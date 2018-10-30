#if INTEL_COLLAB
//===----RTLs/spir/src/rtl.cpp - Target RTLs Implementation ------- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is dual licensed under the MIT and the University of Illinois Open
// Source Licenses. See LICENSE.txt for details.
//
//===----------------------------------------------------------------------===//
//
// This file is modified from https://github.com/daniel-schuermann/openmp.git.
// Thanks to Daniel Scheuermann, the author of rtl.cpp.
//
// RTL for SPIR-V/OpenCL machine
//
//===----------------------------------------------------------------------===//


#include <cassert>
#include <cstring>
#include <cstdlib>
#include <list>
#include <vector>
#include <string>
#include <fstream>
#include <gelf.h>
#include <CL/cl.h>
#include <dlfcn.h>

#include "omptargetplugin.h"

#ifndef TARGET_NAME
#define TARGET_NAME OPENCL
#endif

#define GETNAME2(name) #name
#define GETNAME(name) GETNAME2(name)
// TODO: The DP macro will be cleaned up later.
#define DP(...) DEBUGP("Target " GETNAME(TARGET_NAME) " RTL", __VA_ARGS__)

// TODO: The current implementation only supports one device. It will be
// extended in the future.
#define NUMBER_OF_DEVICES 1
#define OFFLOADSECTIONNAME ".omp_offloading.entries"

//#pragma OPENCL EXTENSION cl_khr_spir : enable

/// Keep entries table per device.
struct FuncOrGblEntryTy {
    __tgt_target_table Table;
    std::vector<__tgt_offload_entry> Entries;
    std::vector<cl_kernel> Kernels;
};

/// Class containing all the device information.
class RTLDeviceInfoTy {

public:
    cl_uint numDevices;
    cl_platform_id platformId;
    // per device information
    std::vector<cl_device_id> deviceIDs;
    std::vector<int32_t> maxWorkGroups;
    std::vector<cl_context> CTX;
    std::vector<cl_command_queue> Queues;
    std::vector<FuncOrGblEntryTy> FuncGblEntries;

    RTLDeviceInfoTy() {
      DP("Start initializing OpenCL\n");
      // get available platforms
      cl_uint platformIdCount = 0;
      clGetPlatformIDs(0, nullptr, &platformIdCount);
      std::vector<cl_platform_id> platformIds(platformIdCount);
      clGetPlatformIDs(platformIdCount, platformIds.data(), nullptr);

      // check version and devices
      for (cl_platform_id id : platformIds) {
        char buffer[128];
        clGetPlatformInfo(id, CL_PLATFORM_VERSION, 128, buffer, NULL);
        if(strncmp("OpenCL 2.1 ", buffer, 14)) {
          continue;
        }
        DP("cl platform version is %s\n", buffer);

        clGetDeviceIDs(id, CL_DEVICE_TYPE_ALL, 0, nullptr, &numDevices);
        deviceIDs.resize(numDevices);
        clGetDeviceIDs(id, CL_DEVICE_TYPE_ALL, numDevices, deviceIDs.data(), nullptr);
        DP("Found %d OpenCL devices\n", numDevices);

        maxWorkGroups.resize(numDevices);
        CTX.resize(numDevices);
        Queues.resize(numDevices);
        FuncGblEntries.resize(numDevices);
        platformId = id;

        // get device specific information
        for (unsigned i = 0; i < numDevices; i++) {
          cl_device_id deviceId = deviceIDs[i];
          clGetDeviceInfo(deviceId, CL_DEVICE_NAME, 128, buffer, nullptr);
          DP("Device#%d: %s\n", i, buffer);
          clGetDeviceInfo(deviceId, CL_DEVICE_MAX_COMPUTE_UNITS, 4, &maxWorkGroups[i], nullptr);
          DP("max WGs is: %d\n", maxWorkGroups[i]);

#ifdef OMPTARGET_DEBUG
          cl_uint addressmode;
          clGetDeviceInfo(deviceId, CL_DEVICE_ADDRESS_BITS, 4, &addressmode, nullptr);
          DP("addressing mode is %d bit\n", addressmode);
#endif
        }
        //return;
      }
      //numDevices = 0;
      //DP("No OpenCL devices found.\n");
    }
};

static RTLDeviceInfoTy DeviceInfo;

#ifdef __cplusplus
extern "C" {
#endif

int32_t __tgt_rtl_is_valid_binary(__tgt_device_image *image) {
  uint32_t magicWord = *(uint32_t *)image->ImageStart;
  // compare magic word in little endian and big endian:
  return magicWord == 0x07230203 || magicWord == 0x03022307;
}

int32_t __tgt_rtl_number_of_devices() { return DeviceInfo.numDevices; } //fixme

int32_t __tgt_rtl_init_device(int32_t device_id) {

  cl_int status;
  DP("Initialize OpenCL device\n");

  // create context
  cl_context_properties props[] = {CL_CONTEXT_PLATFORM, (cl_context_properties) DeviceInfo.platformId, 0};
  DeviceInfo.CTX[device_id] = clCreateContext(props, 1, &DeviceInfo.deviceIDs[device_id], nullptr, nullptr, &status);
  if (status != CL_SUCCESS) {
    DP("OpenCL Error: Failed to create context: %d\n", status);
    return OFFLOAD_FAIL;
  }

  DeviceInfo.Queues[device_id] = clCreateCommandQueueWithProperties(
          DeviceInfo.CTX[device_id], DeviceInfo.deviceIDs[device_id], nullptr, &status);
  if (status != 0) {
    DP("OpenCL Error: Failed to create CommandQueue: %d\n", status);
    return OFFLOAD_FAIL;
  }

  return OFFLOAD_SUCCESS;
}

__tgt_target_table *__tgt_rtl_load_binary(int32_t device_id,
                                          __tgt_device_image *image) {

  DP("Dev %d: load binary from " DPxMOD " image\n", device_id,
     DPxPTR(image->ImageStart));

  assert(device_id >= 0 && device_id < NUMBER_OF_DEVICES && "bad dev id");

  size_t ImageSize = (size_t)image->ImageEnd - (size_t)image->ImageStart;
  size_t NumEntries = (size_t)(image->EntriesEnd - image->EntriesBegin);
  DP("Expecting to have %zd entries defined.\n", NumEntries);

#if 0
  // For debugging purposes, we can write out the spir binary
  char tmp_name[] = "/tmp/tmpfile_XXXXXX";
  int tmp_fd = mkstemp(tmp_name);

  if (tmp_fd == -1) {
    return NULL;
  }

  FILE *ftmp = fdopen(tmp_fd, "wb");

  if (!ftmp) {
    return NULL;
  }

  fwrite(image->ImageStart, ImageSize, 1, ftmp);
  fclose(ftmp);
  DP("written to tmp\n")
#endif

  // create Program
  cl_int status;
  cl_program program[3];
  cl_uint num_programs = 0;

  // Create program for the device RTL if it exits.
  Dl_info rtl_info;

  if (dladdr(&DeviceInfo, &rtl_info)) {
    std::string device_rtl_base = "libomptarget-opencl.a";
    std::string device_rtl_path = rtl_info.dli_fname;
    size_t split = device_rtl_path.find_last_of("/\\");
    device_rtl_path.replace(split + 1, std::string::npos, device_rtl_base);
    std::ifstream device_rtl(device_rtl_path, std::ios::binary);

    if (device_rtl.is_open()) {
      DP("Found device RTL: %s\n", device_rtl_path.c_str());
      device_rtl.seekg(0, device_rtl.end);
      int device_rtl_len = device_rtl.tellg();
      std::string device_rtl_bin(device_rtl_len, '\0');
      device_rtl.seekg(0);
      if (!device_rtl.read(&device_rtl_bin[0], device_rtl_len)) {
        DP("I/O Error: Failed to read device RTL.\n");
        return NULL;
      }

      program[0] = clCreateProgramWithIL(DeviceInfo.CTX[device_id],
                                         device_rtl_bin.c_str(), device_rtl_len,
                                         &status);
      if (status != CL_SUCCESS) {
        DP("OpenCL Error: Failed to create device RTL from IL: %d\n", status);
        return NULL;
      }

      status = clCompileProgram(program[0], 0, nullptr, nullptr, 0, nullptr,
                                nullptr, nullptr, nullptr);
      if (status != CL_SUCCESS) {
        DP("OpenCL Error: Failed to compile device RTL: %d\n", status);
        return NULL;
      }
      num_programs++;
    }
  }

  // Create program for the target regions.
  program[1] = clCreateProgramWithIL(DeviceInfo.CTX[device_id],
                                     image->ImageStart, ImageSize, &status);
  if (status != 0) {
    DP("OpenCL Error: Failed to create program: %d\n", status);
    return NULL;
  }

  status = clCompileProgram(program[1], 0, nullptr, nullptr, 0, nullptr,
                            nullptr, nullptr, nullptr);
  if (status != CL_SUCCESS) {
    DP("OpenCL Error: Failed to compile program: %d\n", status);
    return NULL;
  }
  num_programs++;

  if (num_programs < 2)
    DP("Skipped device RTL.\n");

  program[2] = clLinkProgram(DeviceInfo.CTX[device_id], 1,
                             &DeviceInfo.deviceIDs[device_id], nullptr,
                             num_programs, &program[0], nullptr, nullptr,
                             &status);
  if (status != CL_SUCCESS) {
    DP("OpenCL Error: Failed to link program: %d\n", status);
    return NULL;
  } else {
    DP("OpenCL: Successfully linked program.\n");
  }

  // create kernel and target entries
  DeviceInfo.FuncGblEntries[device_id].Entries.resize(NumEntries);
  DeviceInfo.FuncGblEntries[device_id].Kernels.resize(NumEntries);
  std::vector<__tgt_offload_entry>& entries = DeviceInfo.FuncGblEntries[device_id].Entries;
  std::vector<cl_kernel>& kernels = DeviceInfo.FuncGblEntries[device_id].Kernels;
  for(unsigned i = 0; i< NumEntries; i++) {
    char *name = image->EntriesBegin[i].name;
    kernels[i] = clCreateKernel(program[2], name, &status);
    if (status != 0) {
      DP("OpenCL Error: Failed to create kernel %s, %d\n", name, status);
      return NULL;
    }
    entries[i].addr = &kernels[i];
    entries[i].name = name;
  }

  __tgt_target_table &table = DeviceInfo.FuncGblEntries[device_id].Table;
  table.EntriesBegin = &(entries[0]);
  table.EntriesEnd = &(entries[entries.size()]);
  return &table;
}

void *__tgt_rtl_data_alloc(int32_t device_id, int64_t size, void *hst_ptr) {
  cl_int status;
  cl_mem mem = clCreateBuffer(DeviceInfo.CTX[device_id], CL_MEM_READ_WRITE, size, NULL, &status);
  if (status != CL_SUCCESS) {
    DP("OpenCL Error: Failed to allocate memory: %d\n", status);
    return NULL;
  }
  return mem;
}

int32_t __tgt_rtl_data_submit(int32_t device_id, void *tgt_ptr, void *hst_ptr,
                              int64_t size) {
  cl_int status = clEnqueueWriteBuffer(DeviceInfo.Queues[device_id], (cl_mem) tgt_ptr, CL_TRUE, 0, size, hst_ptr, 0,
                                       nullptr, nullptr);
  if (status != CL_SUCCESS) {
    DP("OpenCL Error: Failed to write buffer: %d\n", status);
    return OFFLOAD_FAIL;
  }
  return OFFLOAD_SUCCESS;
}

int32_t __tgt_rtl_data_retrieve(int32_t device_id, void *hst_ptr, void *tgt_ptr,
                                int64_t size) {
  cl_int status = clEnqueueReadBuffer(DeviceInfo.Queues[device_id], (cl_mem) tgt_ptr, CL_TRUE, 0, size, hst_ptr, 0,
                                       nullptr, nullptr);
  if (status != CL_SUCCESS) {
    DP("OpenCL Error: Failed to read buffer: %d\n", status);
    return OFFLOAD_FAIL;
  }
  return OFFLOAD_SUCCESS;
}

int32_t __tgt_rtl_data_delete(int32_t device_id, void *tgt_ptr) {
  if(cl_int status = clReleaseMemObject((cl_mem) tgt_ptr) != CL_SUCCESS) {
    DP("OpenCL Error: Failed to release buffer: %d\n", status);
    return OFFLOAD_FAIL;
  }
  return OFFLOAD_SUCCESS;
}

int32_t __tgt_rtl_run_target_team_region(int32_t device_id, void *tgt_entry_ptr,
                                         void **tgt_args, ptrdiff_t *tgt_offsets, int32_t arg_num, int32_t team_num,
                                         int32_t thread_limit, uint64_t loop_tripcount /*not used*/) {
  cl_int status;
  cl_kernel * kernel = static_cast<cl_kernel *> (tgt_entry_ptr);
  // debug...
#ifdef OMPTARGET_DEBUG
  char buffer[128];
  clGetKernelInfo(*kernel, CL_KERNEL_FUNCTION_NAME, 128, buffer, nullptr);
  cl_uint n;
  clGetKernelInfo(*kernel, CL_KERNEL_NUM_ARGS, sizeof(cl_uint), &n, nullptr);
  DP("number of kernel parameters: %d\n", n);
  DP("number of arguments: %d\n", arg_num);
#endif

  // set kernel args
  std::vector<void *> ptrs(arg_num);
  for (int32_t i = 0; i < arg_num; ++i) {
    ptrs[i] = (void *)((intptr_t)tgt_args[i] + tgt_offsets[i]);
    status = clSetKernelArg(*kernel, i, /*sizeof(intptr_t) + tgt_offsets[i]*/ sizeof(cl_mem), &ptrs[i]);
    if (status != CL_SUCCESS) {
      DP("OpenCL Error: Failed to set kernel arg %d: %d\n", i, status);
      return OFFLOAD_FAIL;
    } else {
      DP("OpenCL: Kernel Arg %d set successfully\n", i);
    }
  }

  size_t global_work_size;
  size_t local_work_size;
  // calculate number of threads in each team:
  clGetKernelWorkGroupInfo(*kernel, DeviceInfo.deviceIDs[device_id], CL_KERNEL_PREFERRED_WORK_GROUP_SIZE_MULTIPLE,
                           sizeof(size_t), &local_work_size, nullptr);

  // calculate total number of threads to execute the kernel:
  if (thread_limit && team_num)
    global_work_size = std::min(global_work_size, (size_t)thread_limit);
  else if (thread_limit)
    global_work_size = thread_limit;
  else if (team_num)
    global_work_size = local_work_size * team_num;
  else
    global_work_size = local_work_size * DeviceInfo.maxWorkGroups[device_id] * 64; // have sane defaults

  // run kernel:
  DP("thread limit is %d, team num is %d\n", thread_limit, team_num);
  DP("global work size is %zd\n", global_work_size);
  DP("local work size is: %zd\n", local_work_size);

  status = clEnqueueNDRangeKernel(
          DeviceInfo.Queues[device_id], *kernel, 1, nullptr, &global_work_size, &local_work_size, 0, nullptr, nullptr);
  if (status != CL_SUCCESS) {
    DP("OpenCL Error: Failed to enqueue kernel: %d\n", status);
    return OFFLOAD_FAIL;
  }

  DP("OpenCL: Started executing kernel.\n");
  status = clFinish(DeviceInfo.Queues[device_id]);
  if (status != CL_SUCCESS) {
    DP("OpenCL Error: Failed to execute kernel: %d\n", status);
  } else {
    DP("OpenCL: Successfully finished kernel execution.\n")
  }

  return OFFLOAD_SUCCESS;
}

int32_t __tgt_rtl_run_target_region(int32_t device_id, void *tgt_entry_ptr,
                                    void **tgt_args, ptrdiff_t *tgt_offsets, int32_t arg_num) {
  // use one team!
  return __tgt_rtl_run_target_team_region(device_id, tgt_entry_ptr, tgt_args,
                                          tgt_offsets, arg_num, 1, 0, 0);
}

#ifdef __cplusplus
}
#endif
#endif // INTEL_COLLAB
