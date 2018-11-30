//===------ plugins/csa/src/rtl.cpp - CSA RTLs Implementation - C++ -*-----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is dual licensed under the MIT and the University of Illinois Open
// Source Licenses. See LICENSE.txt for details.
//
//===----------------------------------------------------------------------===//
//
// RTL for CSA UMR.
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <dlfcn.h>
#include <forward_list>
#include <fstream>
#include <iomanip>
#include <list>
#include <memory>
#include <mutex>
#include <sstream>
#include <link.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

#include "omptargetplugin.h"
#include "umr.h"
#include "elf.h"

#ifdef OMPTARGET_DEBUG
static int DebugLevel = 0;

#define DP(Level, ...)                                                     \
  do {                                                                     \
    if (DebugLevel >= Level) {                                             \
      fprintf(stderr, "CSA  (HOST)  --> ");                                \
      fprintf(stderr, __VA_ARGS__);                                        \
      fflush(nullptr);                                                     \
    }                                                                      \
  } while (false)
#else
#define DP(Level, ...)                                                     \
  {}
#endif

#define NUMBER_OF_DEVICES 1
#define OFFLOADSECTIONNAME ".omp_offloading.entries"
#define CSA_CODE_SECTION ".csa.code"

// ENVIRONMENT VARIABLES

// If defined, suppresses use of assembly embedded in the binary and specifies
// the file to use instead
#define ENV_ASSEMBLY_FILE "CSA_ASSEMBLY_FILE"

// Variable value has the following format
//   CSA_ASSEMBLY_FILE=<file>[:<entry list>][;<file>[:<entry list>]]
//
// where
//   <file>       A path to CSA aseembly file.
//   <entry list> Comma-separated list of entries defined in the assembly file.
//                For these entries plugin will use assembly from the file
//                instead of compiler generated assembly.
//
// If there is no entry list, assembly file is supposed to define all entries
// which program will execute on CSA.

static std::string AsmFile;
using String2StringMap = std::unordered_map<std::string, std::string>;
static std::unique_ptr<String2StringMap> Entry2AsmFile;

// Specifies that the tool should display the compilation command
// being generated
#define ENV_VERBOSE "CSA_VERBOSE"
static bool Verbosity;

// If defined, dumps the simulator statistics after each offloaded procedure
// is run
#define ENV_DUMP_STATS "CSA_DUMP_STATS"
static bool DumpStats;

// If defined all stats for a thread are run in a single CSA instance and
// dumped in a single .stat file (if CSA_DUMP_STATS is defined)
#define ENV_MERGE_STATS "CSA_MERGE_STATS"
static bool MergeStats;

// If defined, leave the temporary files on disk in the user's directory
#define ENV_SAVE_TEMPS "CSA_SAVE_TEMPS"
static bool SaveTemps;

// If defined, specifies temporary file prefix. If not defined, defaults
// to process name with "-csa" appended. No effect if CSA_SAVE_TEMPS is
// not defined
#define ENV_TEMP_PREFIX "CSA_TEMP_PREFIX"
static std::string TempPrefix;

// Create temporary file. Returns file name if successfull or empty string
// otherwise.
static std::string makeTempFile() {
  char Template[] = "/tmp/tmpfile_XXXXXX";
  int FD = mkstemp(Template);
  if (FD < 0) {
    DP(1, "Error creating temporary file: %s\n", strerror(errno));
    return "";
  }
  close(FD);
  return Template;
}

namespace {

// Represents a dynamic library which is loaded for this target.
class DynLibTy {
  std::string FileName;
  void *Handle;

public:
  explicit DynLibTy(const char *Data, size_t Size) : Handle(nullptr) {
    // Create temporary file for the dynamic library
    FileName = makeTempFile();
    if (FileName.empty())
      return;

    // Write library contents to the file.
    std::ofstream OFS(FileName, std::ofstream::binary | std::ofstream::trunc);
    if (!OFS || !OFS.write(Data, Size)) {
      DP(1, "Error while writing to a temporary file %s\n", FileName.c_str());
      return;
    }
    OFS.close();

    // And finally load the library.
    Handle = dlopen(FileName.c_str(), RTLD_LAZY);
  }

  DynLibTy(DynLibTy &&That) {
    std::swap(FileName, That.FileName);
    std::swap(Handle, That.Handle);
  }

  ~DynLibTy() {
    if (Handle) {
      dlclose(Handle);
      Handle = nullptr;
    }
    if (!SaveTemps && !FileName.empty()) {
      remove(FileName.c_str());
      FileName.clear();
    }
  }

  DynLibTy& operator=(DynLibTy &&That) {
    if (FileName == That.FileName)
      FileName.clear();
    if (Handle == That.Handle)
      Handle = nullptr;
    std::swap(FileName, That.FileName);
    std::swap(Handle, That.Handle);
    return *this;
  }

  operator bool() const {
    return Handle != nullptr;
  }

  const char* getError() const {
    return dlerror();
  }

  const std::string& getName() const {
    return FileName;
  }

  Elf64_Addr getBase() const {
    assert(Handle && "invalid handle");
    return reinterpret_cast<struct link_map*>(Handle)->l_addr;
  }

  DynLibTy(const DynLibTy &) = delete;
  DynLibTy& operator=(const DynLibTy &) = delete;
};

// Elf template specialization for CSA (so far it fully matches x86_64).
using CSAElf =
  Elf<EM_X86_64, Elf64_Ehdr, Elf64_Phdr, Elf64_Shdr, Elf64_Rela, Elf64_Sym>;

} // anonymous namespace

#ifdef _MSC_BUILD
static
std::string get_process_name() {
  char buf[MAX_PATH];
  char name[_MAX_FNAME];
  GetModuleFileName(NULL, buf, MAX_PATH);
  _splitpath_s(buf, NULL, 0, NULL, 0, name, _MAX_FNAME, NULL, 0);
  return name;
#else
static
std::string get_process_name() {
  char buf[2048];

  int ret = readlink("/proc/self/exe", buf, sizeof(buf)-1);
  if (-1 == ret) {
    fprintf(stderr, "Failed to get image name");
    return "unknown-process";
  }

  buf[ret] = '\0';
  char* name = strrchr(buf, '/');
  if (NULL == name) {
    return buf;
  } else {
    return name+1;
  }
}
#endif

#ifdef OMPTARGET_DEBUG
// Return string describing UMR error.
static const char* getUmrErrorStr(CsaUmrErrors E) {
  switch (E) {
    case kCsaUmrOK:
      return "no error";
    case kCsaUmrErrorContextBusy:
      return "UMR context is being used by another thread";
    case kCsaUmrErrorContextGroupLimit:
      return "too many UMR contexts in a group";
    case kCsaUmrErrorNotContextGroup:
      return "call to UMR contexts from different groups";
    default:
      break;
  }
  return "unknown UMR error";
}
#endif // OMPTARGET_DEBUG

// Error checking wrapper for the CsaUmrCreateContext API. In case of error,
// prints debugging message and returns nullptr. Otherwise returns created
// context.
static CsaUmrContext* createContext(const CsaUmrContextAttributes *Attrs,
                                    CsaUmrHandler *Handler) {
  CsaUmrContext *Ctxt = nullptr;
  auto E = CsaUmrCreateContext(Attrs, Handler, &Ctxt);
  if (E) {
    DP(1, "Error creating UMR context - %s\n", getUmrErrorStr(E));
    return nullptr;
  }
  return Ctxt;
}

// Error checking wrapper for the CsaUmrBindGraphFromFile API. In case of error
// prints debugging message and returns nullptr. Otherwise returns binded graph.
static CsaUmrBoundGraph* bindGraph(CsaUmrContext *Ctxt, const char *Path) {
  CsaUmrBoundGraph *Graph = nullptr;
  auto E = CsaUmrBindGraphFromFile(Ctxt, Path, &Graph);
  if (E) {
    DP(1, "Failed to bind CSA graph - %s\n", getUmrErrorStr(E));
    return nullptr;
  }
  return Graph;
}

// Error checking wrapper for the CsaUmrCall API. In case of error prints
// debugging message and returns false. Otherwise returns true.
static bool callGraph(CsaUmrBoundGraph *Graph, const char *Entry,
                      const std::vector<void*> &Args) {
  CsaUmrCallInfo CI = { 0 };
  CI.flags = kCsaUmrCallEntryByName;
  CI.graph = Graph;
  CI.entry_name = Entry;
  CI.num_inputs = Args.size();
  CI.inputs = reinterpret_cast<const CsaArchValue64*>(Args.data());

  auto E = CsaUmrCall(&CI, 0);
  if (E) {
    DP(1, "Error calling CSA graph - %s\n", getUmrErrorStr(E));
    return false;
  }
  return true;
}

namespace {

/// Class containing all the device information.
class RTLDeviceInfoTy {
  // For function entries target address in the offload entry table for CSA
  // will point to this object. It is a pair of two null-termminated strings
  // where the first string is the offload entry name, and the second is the
  // name of file which contains entry's assembly.
  using EntryAddr = std::pair<const char*, const char*>;

  // Structure which represents an offload entry table for CSA binary.
  struct EntryTable : public __tgt_target_table {
    static EntryTable* create(const __tgt_offload_entry *Entries, size_t Size) {
      std::unique_ptr<EntryTable> Table(new EntryTable());
      if (!Table->construct(Entries, Size))
        return nullptr;
      return Table.release();
    }

    ~EntryTable() {
      if (!SaveTemps)
        for (const auto &File : Addr2AsmFile)
          remove(File.second.c_str());
    }

  private:
    bool construct(const __tgt_offload_entry *Table, size_t Size) {
      Entries.resize(Size);
      for (size_t I = 0u; I < Size; ++I) {
        Entries[I] = Table[I];
        if (!Entries[I].size) {
          // Function entry. Create an EntryAddr instance for it and assign
          // its address the the entry address.
          DP(2, "Entry[%lu]: name %s\n", I, Entries[I].name);

          const auto *FileName = getEntryFile(Entries[I]);
          if (!FileName)
            return false;

          Addresses.emplace_front(Entries[I].name, FileName);
          Entries[I].addr = &Addresses.front();
        }
        else
          // It is a data entry. Keep entry address as is. It is supposed to be
          // the same as host's address, but if not, we can always propagate it
          // from the host table.
          DP(2, "Entry[%lu]: name %s, address %p, size %lu\n", I,
             Entries[I].name, Entries[I].addr, Entries[I].size);
      }
      EntriesBegin = Entries.data();
      EntriesEnd = Entries.data() + Size;
      return true;
    }

    const char *getEntryFile(const __tgt_offload_entry &Entry) {
      // There are three possible options for getting assembly for an entry.
      // (1) If we have a single user-defined assembly file, then we use it.
      if (!AsmFile.empty())
        return AsmFile.c_str();

      // (2) Otherwise if there is an entry -> assembly map, try to find
      // assembly file for the given entry.
      if (Entry2AsmFile) {
        auto It = Entry2AsmFile->find(Entry.name);
        if (It != Entry2AsmFile->end())
          return It->second.c_str();
      }

      // (3) Otherwise save the embedded assembly to a file.
      // Check if we have already saved this asm string earlier.
      auto It = Addr2AsmFile.find(Entry.addr);
      if (It != Addr2AsmFile.end())
        return It->second.c_str();

      // We have not seen this entry yet.
      std::string FileName;
      if (SaveTemps) {
        static std::atomic<unsigned int> AsmCount;
        std::stringstream SS;
        SS << TempPrefix << AsmCount++ << ".s";
        FileName = SS.str();

        if (Verbosity)
          fprintf(stderr, "Saving CSA assembly to \"%s\"\n", FileName.c_str());
      }
      else {
        FileName = makeTempFile();
        if (FileName.empty())
          return nullptr;
      }

      // Save assembly.
      DP(3, "Saving CSA assembly to \"%s\"\n", FileName.c_str());
      std::ofstream OFS(FileName, std::ofstream::trunc);
      if (!OFS || !(OFS << static_cast<const char*>(Entry.addr))) {
        DP(1, "Error while saving assembly to a file %s\n", FileName.c_str());
        return nullptr;
      }
      OFS.close();

      // And update asm files map.
      auto Res = Addr2AsmFile.emplace(Entry.addr, std::move(FileName));
      assert(Res.second && "unexpected entry in the map");
      return Res.first->second.c_str();
    }

  private:
    std::vector<__tgt_offload_entry> Entries;
    std::forward_list<EntryAddr> Addresses;
    std::unordered_map<void*, std::string> Addr2AsmFile;
  };

  // An object which contains all data for a single CSA binary - dynamic library
  // object and the entry table for this binary.
  using CSAImage = std::pair<DynLibTy, std::unique_ptr<EntryTable>>;

  // Keep a list of loaded CSA binaries. This list is always accessed from a
  // single thread so, there is no need to do any synchronization while
  // accessing/modifying it.
  std::forward_list<CSAImage> CSAImages;

public:
  // Loads given CSA image and returns the image's entry table.
  __tgt_target_table* loadImage(const __tgt_device_image *Image) {
    if (!Image)
      return nullptr;

    // Image start and size.
    const char *Start = static_cast<const char*>(Image->ImageStart);
    size_t Size = static_cast<const char*>(Image->ImageEnd) - Start;

    DP(1, "Reading target ELF %p...\n", Start);
    CSAElf Elf;
    if (!Elf.readFromMemory(Start, Size)) {
      DP(1, "Error while parsing target ELF\n");
      return nullptr;
    }

    // Find section with offload entry table.
    const auto *EntriesSec = Elf.findSection(OFFLOADSECTIONNAME);
    if (!EntriesSec) {
      DP(1, "Entries Section Not Found\n");
      return nullptr;
    }
    auto EntriesAddr = EntriesSec->getAddr();
    auto EntriesSize = EntriesSec->getSize();
    DP(1, "Entries Section: address %lx, size %lu\n", EntriesAddr, EntriesSize);

    // Entry table size is expected to match on the host and target sides.
    auto TabSize = EntriesSize / sizeof(__tgt_offload_entry);
    assert(TabSize == size_t(Image->EntriesEnd - Image->EntriesBegin) &&
           "table size mismatch");

    // Create temp file with library contents and load the library.
    DynLibTy DL{Start, Size};
    if (!DL) {
      DP(1, "Error while loading %s - %s\n", DL.getName().c_str(),
         DL.getError());
      return nullptr;
    }
    DP(1, "Saved device binary to %s\n", DL.getName().c_str());

    // Entry table address in the loaded library.
    auto *Tab = reinterpret_cast<__tgt_offload_entry*>(DL.getBase() +
                                                       EntriesAddr);

    // Construct entry table.
    auto *Table = EntryTable::create(Tab, TabSize);
    if (!Table) {
      DP(1, "Error while creating entry table\n");
      return nullptr;
    }

    // Construct new CSA image and insert it into the list.
    CSAImages.emplace_front(std::move(DL), std::unique_ptr<EntryTable>(Table));
    return CSAImages.front().second.get();
  }

public:
  // An object which represents a single OpenMP offload device.
  class Device {
    // Set which keeps addresses for allocated memory.
    class : private std::unordered_set<void*> {
      std::mutex Mutex;

    public:
      void* alloc(size_t Size) {
        void *Ptr = malloc(Size);
        if (!Ptr)
          return nullptr;
        std::lock_guard<std::mutex> Lock(Mutex);
        auto Res = insert(Ptr);
        assert(Res.second && "allocated memory is in the set");
        (void) Res;
        return Ptr;
      }

      void free(void *Ptr) {
        if (!Ptr)
          return;
        {
          std::lock_guard<std::mutex> Lock(Mutex);
          auto It = find(Ptr);
          if (It == end())
            return;
          erase(It);
        }
        free(Ptr);
      }
    } MemoryMap;

  public:
    void* alloc(size_t Size) {
      return MemoryMap.alloc(Size);
    }

    void free(void *Ptr) {
      MemoryMap.free(Ptr);
    }

  public:
    // Data associated with each offload entry - UMR context and a graph.
    using CSAEntry = std::pair<CsaUmrContext*, CsaUmrBoundGraph*>;

    // Maps offload entry to a CSA entry for a thread. No synchronization is
    // necessary for this object because it is accessed and/or modified by one
    // thread only.
    class CSAEntryMap : public std::unordered_map<const EntryAddr*, CSAEntry> {
      CsaUmrContext *Context = nullptr;

    public:
      CsaUmrContext* getContext() const {
        return Context;
      }

    private:
      // Creates CSA UMR context. The way how we do it depends on the MergeStats
      // setting. If MergeStats is on then we are using single context for all
      // entries. Otherwise each entry gets its own context.
      CsaUmrContext* getOrCreateContext() {
        if (!MergeStats)
          return createContext(nullptr, nullptr);

        // When MergeStats is on thread is supposed to run all entries in
        // a single context.
        if (!Context)
          Context = createContext(nullptr, nullptr);
        return Context;
      }

    public:
      CSAEntry* getEntry(const EntryAddr *Addr) {
        auto It = find(Addr);
        if (It != end())
          return &It->second;

        auto *Ctxt = getOrCreateContext();
        if (!Ctxt)
          return nullptr;

        DP(5, "Using assembly from \"%s\" for entry \"%s\"\n",
           Addr->second, Addr->first);

        auto *Graph = bindGraph(Ctxt, Addr->second);
        if (!Graph)
          return nullptr;

        auto Res = this->emplace(Addr, std::make_pair(Ctxt, Graph));
        assert(Res.second && "entry is already in the map");
        return &Res.first->second;
      }
    };

    // Per thread map of CSA entries.
    class ThreadEntryMap :
        public std::unordered_map<std::thread::id, CSAEntryMap> {
      std::mutex Mutex;

    public:
      CSAEntryMap& getEntries() {
        std::lock_guard<std::mutex> Lock(Mutex);
        return (*this)[std::this_thread::get_id()];
      }
    };

  private:
    ThreadEntryMap ThreadEntries;

  public:
    ThreadEntryMap& getThreadEntries() {
      return ThreadEntries;
    }

    bool runFunction(void *Ptr, std::vector<void*> &Args) {
      auto *Addr = static_cast<EntryAddr*>(Ptr);
      auto *Info = ThreadEntries.getEntries().getEntry(Addr);
      if (!Info) {
        DP(1, "Error while creating CSA entry\n");
        return false;
      }

      auto *Name = Addr->first;
      auto *Ctxt = Info->first;
      auto *Graph = Info->second;

      DP(2, "Running function %s with %lu argument(s)\n", Name, Args.size());
      for (size_t I = 0u; I < Args.size(); ++I)
        DP(2, "\tArg[%lu] = %p\n", I, Args[I]);

      unsigned RunNumber;
      int64_t StartCycles;
      if (Verbosity) {
        // Run function counter.
        static std::atomic<unsigned> RunCount;

        RunNumber = RunCount++;
        StartCycles = CsaUmrSimulatorGetCycles(Ctxt);

        fprintf(stderr, "\nRun %u: Running %s on the CSA ..\n",
                RunNumber, Name);
      }

      if (!callGraph(Graph, Name, Args))
        return false;

      if (Verbosity) {
        auto Cycles = CsaUmrSimulatorGetCycles(Ctxt) - StartCycles;
        fprintf(stderr,
                "\nRun %u: %s ran on the CSA in %ld cycles\n\n",
                RunNumber, Name, Cycles);
      }
      return true;
    }
  };

private:
  std::unique_ptr<Device[]> Devices;
  int NumDevices = 0;

public:
  int getNumDevices() const {
    return NumDevices;
  }

  Device& getDevice(int ID) {
    assert(ID >= 0 && ID < getNumDevices() && "bad device ID");
    return Devices[ID];
  }

  const Device& getDevice(int ID) const {
    assert(ID >= 0 && ID < getNumDevices() && "bad device ID");
    return Devices[ID];
  }

public:
  RTLDeviceInfoTy() {
    NumDevices = NUMBER_OF_DEVICES;
    Devices.reset(new Device[NumDevices]);
  }

  ~RTLDeviceInfoTy() {
    std::unordered_map<std::thread::id, int> TID2Num;
    std::string ProcessName;
    int Width = 0;

    if (DumpStats) {
      // Build a map of thread IDs to simple numbers
      int ThreadNum = 0;
      for (int I = 0; I < getNumDevices(); ++I)
        for (const auto &Thr : getDevice(I).getThreadEntries())
          if (TID2Num.find(Thr.first) == TID2Num.end())
            TID2Num[Thr.first] = ThreadNum++;
      Width = ceil(log10(ThreadNum));
      ProcessName = get_process_name();

      // Append MPI rank to the name if the process is running under MPI.
      if (const auto *Rank = getenv("PMI_RANK"))
        ProcessName = ProcessName + "-mpi" + Rank;
    }

    // Finish up - Dump the stats, release the CSA instances
    for (int I = 0; I < getNumDevices(); ++I)
      for (auto &Thr : getDevice(I).getThreadEntries()) {
        auto cleanup = [&](CsaUmrContext *C, const std::string &Entry) {
          if (DumpStats) {
            // Compose a file name using the following template
            // <exe name>-<entry name>-dev<device num>-thr<thread num>
            std::stringstream SS;
            SS << ProcessName << "-" << Entry << "-dev" << I << "-thd"
               << std::setfill('0') << std::setw(Width) << TID2Num[Thr.first];
            CsaUmrSimulatorDumpStatistics(C, SS.str().c_str());
          }
          CsaUmrDeleteContext(C);
        };

        if (auto *C = Thr.second.getContext())
          cleanup(C, "*");
        else
          for (auto &Entry : Thr.second)
            cleanup(std::get<0>(Entry.second), Entry.first->first);
      }
  }
};

} // anonymous namespace

static RTLDeviceInfoTy& getDeviceInfo() {
  static RTLDeviceInfoTy DeviceInfo;
  static std::once_flag InitFlag;

  std::call_once(InitFlag, [&]() {
    // One time initialization
#ifdef OMPTARGET_DEBUG
    if (const char *Str = getenv("LIBOMPTARGET_DEBUG"))
      DebugLevel = std::stoi(Str);
#endif // OMPTARGET_DEBUG
    Verbosity = getenv(ENV_VERBOSE);
    DumpStats = getenv(ENV_DUMP_STATS);
    MergeStats = getenv(ENV_MERGE_STATS);
    SaveTemps = getenv(ENV_SAVE_TEMPS);
    if (SaveTemps) {
      // Temp prefix is in effect only if save temps is set.
      if (const char *Str = getenv(ENV_TEMP_PREFIX))
        TempPrefix = Str;
      else
        TempPrefix = get_process_name();
    }
    if (const auto *Str = getenv(ENV_ASSEMBLY_FILE)) {
      // Parse string which is expected to have the following format
      //   CSA_ASSEMBLY_FILE=<file>[:<entry list>][;<file>[:<entry list>]]
      std::istringstream SSV(Str);
      std::string Value;
      while (std::getline(SSV, Value, ';')) {
        auto EntriesPos = Value.find(':');
        if (EntriesPos == std::string::npos)
          // If no entry list is given, then asm file overrides all entries.
          AsmFile = Value;
        else {
          // Otherwise we have asm file name with a list of entries.
          auto File = Value.substr(0u, EntriesPos);

          // Split entries and put them into the entry map.
          std::istringstream SSE(Value.substr(EntriesPos + 1u));
          std::string Entry;
          while (std::getline(SSE, Entry, ',')) {
            if (!Entry2AsmFile)
              Entry2AsmFile.reset(new String2StringMap());
            Entry2AsmFile->insert({ Entry, File });
          }
        }
      }

      // Check that we do not have AsmFile and Entry2AsmFile both defined.
      if (!AsmFile.empty() && Entry2AsmFile) {
        fprintf(stderr, "ignoring malformed %s setting\n", ENV_ASSEMBLY_FILE);
        AsmFile = "";
        Entry2AsmFile = nullptr;
      }
    }
  });
  return DeviceInfo;
}

// Plugin API implementation.

int32_t __tgt_rtl_is_valid_binary(__tgt_device_image *Image) {
  const char *Start = static_cast<char*>(Image->ImageStart);
  size_t Size = static_cast<char*>(Image->ImageEnd) - Start;

  CSAElf Elf;
  if (!Elf.readFromMemory(Start, Size)) {
    DP(1, "Unable to read ELF!\n");
    return false;
  }

  // So far CSA binary is indistinguishable from x86_64 by looking at ELF
  // machine only. We can slightly enhance this test by checking if given
  // binary contains CSA code section.
  if (!Elf.findSection(CSA_CODE_SECTION)) {
    DP(1, "No CSA code section in the binary\n");
    return false;
  }

  return true;
}

int32_t __tgt_rtl_number_of_devices() {
  return getDeviceInfo().getNumDevices();
}

int32_t __tgt_rtl_init_device(int32_t ID) {
  return OFFLOAD_SUCCESS;
}

__tgt_target_table *__tgt_rtl_load_binary(int32_t ID, __tgt_device_image *Ptr) {
  return getDeviceInfo().loadImage(Ptr);
}

void *__tgt_rtl_data_alloc(int32_t ID, int64_t Size, void *HPtr) {
  if (HPtr)
    return HPtr;
  return getDeviceInfo().getDevice(ID).alloc(Size);
}

int32_t __tgt_rtl_data_submit(int32_t ID, void *TPtr, void *HPtr,
                              int64_t Size) {
  if (TPtr != HPtr)
    memcpy(TPtr, HPtr, Size);
  return OFFLOAD_SUCCESS;
}

int32_t __tgt_rtl_data_retrieve(int32_t ID, void *HPtr, void *TPtr,
                                int64_t Size) {
  if (HPtr != TPtr)
    memcpy(HPtr, TPtr, Size);
  return OFFLOAD_SUCCESS;
}

int32_t __tgt_rtl_data_delete(int32_t ID, void *TPtr) {
  getDeviceInfo().getDevice(ID).free(TPtr);
  return OFFLOAD_SUCCESS;
}

int32_t __tgt_rtl_run_target_team_region(int32_t ID, void *Entry,
    void **Bases, ptrdiff_t *Offsets, int32_t NumArgs, int32_t TeamNum,
    int32_t ThreadLimit, uint64_t LoopTripCount) {
  std::vector<void*> Args(NumArgs);
  for (int32_t I = 0; I < NumArgs; ++I)
    Args[I] = static_cast<char*>(Bases[I]) + Offsets[I];

  if (!getDeviceInfo().getDevice(ID).runFunction(Entry, Args))
    return OFFLOAD_FAIL;
  return OFFLOAD_SUCCESS;
}

int32_t __tgt_rtl_run_target_region(int32_t device_id, void *tgt_entry_ptr,
                                    void **tgt_args, ptrdiff_t *tgt_offsets,
                                    int32_t arg_num) {
  // use one team and one thread.
  return __tgt_rtl_run_target_team_region(device_id, tgt_entry_ptr, tgt_args,
                                          tgt_offsets, arg_num, 1, 1, 0);
}

int32_t
__tgt_rtl_run_target_team_nd_region(int32_t device_id, void *tgt_entry_ptr,
                                    void **tgt_args, ptrdiff_t *tgt_offsets,
                                    int32_t num_args, int32_t num_teams,
                                    int32_t thread_limit, void *loop_desc) {
  return OFFLOAD_FAIL;
}
