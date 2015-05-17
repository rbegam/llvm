from sets import Set
import xml.etree.ElementTree as ET
import re
import os
import sys
import argparse

filelist = ["adxintrin.h", "avx2intrin.h", "avxintrin.h", "bmi2intrin.h", "bmiintrin.h",
"emmintrin.h", "f16cintrin.h", "fmaintrin.h", "ia32intrin.h", "immintrin.h", "lzcntintrin.h", "mmintrin.h",
"mm_malloc.h", "nmmintrin.h", "pmmintrin.h", "popcntintrin.h", "prfchwintrin.h", "rdseedintrin.h", "rtmintrin.h",
"shaintrin.h", "smmintrin.h",  "tmmintrin.h", "wmmintrin.h", "x86intrin.h", "xmmintrin.h", 
"__wmmintrin_aes.h", "__wmmintrin_pclmul.h"]
allIntrinsics = Set([])
intrinsicTech = {}

# Matches a function prototype. This relies on the fact that the coding style used in
# the intrinsic files is:
# static __inline__ <return-type> __attribute__((__always_inline__, __nodebug__))
# _<intrinsic-name>(<param-list>)
funcRegEx = re.compile(r'^(_.*?)\s*\(.*$')

# Matches a macro definition. The first group matches #define, the second
# the intrinsic name, and the third the param list. The fourth and fifth are
# used to distiniguish a multi-line macro from a single-line macro.
macroRegEx = re.compile(r'^(#define\s+)(_.*?)(\(.*?\))([^\\]*)(\\?)')

# Matches a "simple" macro definition, that simply re-defines an intrinsic
# in terms of an equivalent intrinsic, e.g.
# #define _mm_foo _mm_bar
simpleMacroRegEx = re.compile(r'^#define\s+(_.*?)\s')

def handleFunction(line, inFile, outFile):
  # TODO: The original line may have the funcDecl, not the next line...
  # (This is not likely though, given how the intrinsic headers are written)
  outFile.write(line)
  # We expect the next line to start the prototype
  # TODO: There may be more than one line here.
  # (This actually happens for mm_malloc!)
  funcDecl = inFile.readline()
  outFile.write(funcDecl)
  stripped = funcDecl.strip()
  # Check that the line actually contains a function we're interested in
  match = funcRegEx.match(stripped)
  if match is None:
    return
  name = match.group(1)
  tech = intrinsicTech.get(name)
  if tech is None:
    return
  allIntrinsics.remove(name)
  # TODO: This assumes that after an opening brace, there is nothing on the same line
  while not stripped.endswith('{'):
    line = inFile.readline()
    outFile.write(line)
    stripped = line.strip()
  outFile.write('  __builtin_assume(__builtin_has_cpu_feature(_FEATURE_%s));\n' % tech)

def handleMacro(line, inFile, outFile):
  # The "prototype" should be on the same line
  stripped = line.strip()
  match = macroRegEx.match(stripped)
  if match is None:
    outFile.write(line)
    # If this is a "define foo", remove it anyway
    match = simpleMacroRegEx.match(stripped)
    if match is not None:
      name = match.group(1)
      allIntrinsics.discard(name)
    return
  name = match.group(2)
  tech = intrinsicTech.get(name)
  if tech is None:
    outFile.write(line)
    # TODO: Should probably read to the end of the macro instead of returning early!
    return
  allIntrinsics.remove(name)
  # Output the prototype, and add the assume followed by the comma operator
  for i in [1, 2, 3]:
    outFile.write(match.group(i))
  outFile.write(' \\\n  (__builtin_assume(__builtin_has_cpu_feature(_FEATURE_%s)), \\\n' % tech)
  outFile.write(match.group(4))
  if match.group(5) == '':
    outFile.write(')\n')
    return
  # The first line ended with a backslash. Write the backslash back
  outFile.write('\\\n')
  # Now, find the first line that doesn't end with a backslash, 
  # and add the closing paren to it.    
  while True:
    line = inFile.readline()
    if not line.strip().endswith('\\'): break
    outFile.write(line)
  outFile.write(line.rstrip())
  outFile.write(')\n')

# TODO: Missing translations: PREFETCHWT1, TSC, FSGSBASE, MONITOR, RDTSCP
XMLToFeature = {"BMI1" : "BMI", "BMI2" : "BMI", "FP16C" : "F16C", "RDRAND" : "RDRND", "SSE4.1" : "SSE4_1", "SSE4.2" : "SSE4_2",
"PREFETCHWT1" : "GENERIC_IA32", "TSC" : "GENERIC_IA32", "FSGSBASE" : "GENERIC_IA32", "MONITOR" : "GENERIC_IA32", "RDTSCP" : "GENERIC_IA32"}

def parseDataXML(xmlfile):
  root = ET.parse(xmlfile)
  for elem in root.findall("intrinsic"):
    # TODO: Handle elements with more than one CPUID
    ID = elem.find("CPUID")
    tech = elem.get("tech")
    # TODO: Figure out what to do with AVX512
    if (ID is not None and not "512" in tech and not "KNC" in tech and not "SVML" in tech 
      and ID.text != "MPX" and ID.text != "XSAVE" and ID.text != "FXSR"):
      name = elem.get("name")
      intrinsicTech[name] = XMLToFeature.get(ID.text, ID.text)
      allIntrinsics.add(name)

def main():
  argParser = argparse.ArgumentParser(description='Add __builtin_assume() calls to intrinsic header files')
  argParser.add_argument('descfile', help='XML file describing the intrinsics and their feature associations')
  argParser.add_argument('dir', help='Directory where intrinsic header files are found')
  args = argParser.parse_args()
  
  parseDataXML(args.descfile)
  includeString = '#include <__x86intrin_features.h>\n'
  for file in filelist:
    fullName = os.path.join(args.dir, file)
    tempName = fullName + ".tmp"
    os.rename(fullName, tempName)
    with open(tempName, "r") as inFile:
      line = inFile.readline()
      if line == includeString:
        # The file has already been processed by a previous invocation
        inFile.close()
        os.rename(tempName, fullName)
        continue
      print "Adding assumes to " + file
      with open(fullName, "w") as outFile:
        outFile.write(includeString);
        while True:
          if not line: break
          # TODO: Do better checking here
          if line.startswith("static __inline"):
              handleFunction(line, inFile, outFile)
          elif line.startswith("#define"):
              handleMacro(line, inFile, outFile)
          else: outFile.write(line)
          line = inFile.readline()
    os.remove(tempName)

if __name__ == "__main__":
  sys.exit(main())

