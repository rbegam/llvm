# -*- Python -*-

import os
import platform
import re
import subprocess
import tempfile
from distutils.spawn import find_executable

import lit.formats
import lit.util

from lit.llvm import llvm_config

# Configuration file for the 'lit' test runner.

# name: The name of this test suite.
config.name = 'SYCL'

# testFormat: The test format to use to interpret tests.
#
# For now we require '&&' between commands, until they get globally killed and
# the test runner updated.
config.test_format = lit.formats.ShTest()

# suffixes: A list of file extensions to treat as test files.
config.suffixes = ['.c', '.cpp'] #add .spv. Currently not clear what to do with those

config.excludes = ['CMakeLists.txt', 'run_tests.sh', 'README.txt', 'Inputs']

# test_source_root: The root path where tests are located.
config.test_source_root = os.path.dirname(__file__)

# test_exec_root: The root path where tests should be run.
config.test_exec_root = os.path.join(config.sycl_obj_root, 'test')

if platform.system() == "Linux":
    config.available_features.add('linux')
    # Propagate 'LD_LIBRARY_PATH' through the environment.
    if 'LD_LIBRARY_PATH' in os.environ:
        config.environment['LD_LIBRARY_PATH'] = os.path.pathsep.join((config.environment['LD_LIBRARY_PATH'], config.llvm_build_libs_dir))
    else:
        config.environment['LD_LIBRARY_PATH'] = config.llvm_build_libs_dir
else:
    config.available_features.add('windows')
    if 'LIB' in os.environ:
        config.environment['LIB'] = os.path.pathsep.join((config.environment['LIB'], config.llvm_build_libs_dir))
    else:
        config.environment['LIB'] = config.llvm_build_libs_dir

    if 'PATH' in os.environ:
        config.environment['PATH'] = os.path.pathsep.join((config.environment['PATH'], config.llvm_build_bins_dir))
    else:
        config.environment['PATH'] = config.llvm_build_bins_dir

# propagate the environment variable OCL_ICD_FILANEMES to use proper runtime.
if 'OCL_ICD_FILENAMES' in os.environ:
    config.environment['OCL_ICD_FILENAMES'] = os.environ['OCL_ICD_FILENAMES']

# INTEL_CUSTOMIZATION
config.available_features.add('opencl-interop')
if 'SYCL_BE' in os.environ:
    config.environment['SYCL_BE'] = os.environ['SYCL_BE']
    # There is no direct OpenCL interop when not running OpenCL BE.
    if config.environment['SYCL_BE'] == "PI_OTHER":
        config.available_features.remove('opencl-interop')

if 'SYCL_DEVICE_WHITE_LIST' in os.environ:
    config.environment['SYCL_DEVICE_WHITE_LIST'] = os.environ['SYCL_DEVICE_WHITE_LIST']
# end INTEL_CUSTOMIZATION

config.substitutions.append( ('%clang_cc1', ' ' + config.clang + ' -cc1 ') )
# INTEL_CUSTOMIZATION
# Propagate --gcc-toolchain if we are overriding system installed gcc.
if 'ICS_GCCBIN' in os.environ:
    config.substitutions.append( ('%clangxx', ' ' + config.clangxx
        + ' --gcc-toolchain=' + os.path.normpath(os.path.join(os.environ['ICS_GCCBIN'], "..") )
        + ' -I'+config.opencl_include ) )
    config.substitutions.append( ('%clang', ' ' + config.clang
        + ' --gcc-toolchain=' + os.path.normpath(os.path.join(os.environ['ICS_GCCBIN'], "..") )
        + ' -I'+config.opencl_include ) )
else:
    config.substitutions.append( ('%clangxx', ' ' + config.clangxx + ' -I'+config.opencl_include ) )
    config.substitutions.append( ('%clang_cl', ' ' + config.clang_cl + ' /I '+config.opencl_include ) )
    config.substitutions.append( ('%clang', ' ' + config.clang + ' -I'+config.opencl_include ) )
# end INTEL_CUSTOMIZATION
config.substitutions.append( ('%llvm_build_libs_dir',  config.llvm_build_libs_dir ) )
config.substitutions.append( ('%opencl_include',  config.opencl_include ) )
config.substitutions.append( ('%sycl_include',  config.sycl_include ) )

tools = ['llvm-spirv']
tool_dirs = [config.llvm_tools_dir]
llvm_config.add_tool_substitutions(tools, tool_dirs)

# INTEL_CUSTOMIZATION
# Ask llvm-config about assertions.
llvm_config.feature_config([('--assertion-mode', {'ON': 'asserts'})])
# end INTEL_CUSTOMIZATION

if "opencl-aot" in config.llvm_enable_projects:
    if 'PATH' in os.environ:
        print("Adding path to opencl-aot tool to PATH")
        os.environ['PATH'] = os.path.pathsep.join((os.getenv('PATH'), config.llvm_build_bins_dir))

get_device_count_by_type_path = os.path.join(config.llvm_binary_dir,
    "bin", "get_device_count_by_type")

def getDeviceCount(device_type):
# INTEL_CUSTOMIZATION
    # If SYCL_LIT_USE_HOST_ONLY is unset or set to something not equal to FALSE,
    # then return 0 to prohibit using all devices except HOST.
    host_only = os.getenv('SYCL_LIT_USE_HOST_ONLY', 'true').lower()
    if host_only != '0' and host_only != 'false' and host_only != 'no':
        return 0
# end INTEL_CUSTOMIZATION

    process = subprocess.Popen([get_device_count_by_type_path, device_type],
        stdout=subprocess.PIPE)
    (output, err) = process.communicate()
    exit_code = process.wait()
    if exit_code == 0:
        result = output.decode().replace('\n', '').split(':', 1)
        try:
            value = int(result[0])
        except ValueError:
            value = 0
            print("getDeviceCount {TYPE}:Cannot get value from output.".format(
                TYPE=device_type))
        if len(result) > 1 and len(result[1]):
            print("getDeviceCount {TYPE}:{MSG}".format(
                TYPE=device_type, MSG=result[1]))
        if err:
            print("getDeviceCount {TYPE}:{ERR}".format(
                TYPE=device_type, ERR=err))
        return value
    return 0


# INTEL_CUSTOMIZATION
cpu_run_substitute = "true"
cpu_run_on_linux_substitute = "true "
# end INTEL_CUSTOMIZATION
cpu_check_substitute = ""
cpu_check_on_linux_substitute = ""
if getDeviceCount("cpu"):
    print("Found available CPU device")
    cpu_run_substitute = "env SYCL_DEVICE_TYPE=CPU "
    cpu_check_substitute = "| FileCheck %s"
    config.available_features.add('cpu')
    if platform.system() == "Linux":
        cpu_run_on_linux_substitute = "env SYCL_DEVICE_TYPE=CPU "
        cpu_check_on_linux_substitute = "| FileCheck %s"
config.substitutions.append( ('%CPU_RUN_PLACEHOLDER',  cpu_run_substitute) )
config.substitutions.append( ('%CPU_RUN_ON_LINUX_PLACEHOLDER',  cpu_run_on_linux_substitute) )
config.substitutions.append( ('%CPU_CHECK_PLACEHOLDER',  cpu_check_substitute) )
config.substitutions.append( ('%CPU_CHECK_ON_LINUX_PLACEHOLDER',  cpu_check_on_linux_substitute) )

# INTEL_CUSTOMIZATION
gpu_run_substitute = "true"
gpu_run_on_linux_substitute = "true "
# end INTEL_CUSTOMIZATION
gpu_check_substitute = ""
gpu_check_on_linux_substitute = ""
if getDeviceCount("gpu"):
    print("Found available GPU device")
    gpu_run_substitute = " env SYCL_DEVICE_TYPE=GPU "
    gpu_check_substitute = "| FileCheck %s"
    config.available_features.add('gpu')
    if platform.system() == "Linux":
        gpu_run_on_linux_substitute = "env SYCL_DEVICE_TYPE=GPU "
        gpu_check_on_linux_substitute = "| FileCheck %s"
config.substitutions.append( ('%GPU_RUN_PLACEHOLDER',  gpu_run_substitute) )
config.substitutions.append( ('%GPU_RUN_ON_LINUX_PLACEHOLDER',  gpu_run_on_linux_substitute) )
config.substitutions.append( ('%GPU_CHECK_PLACEHOLDER',  gpu_check_substitute) )
config.substitutions.append( ('%GPU_CHECK_ON_LINUX_PLACEHOLDER',  gpu_check_on_linux_substitute) )

# INTEL_CUSTOMIZATION
acc_run_substitute = "true"
# end INTEL_CUSTOMIZATION
acc_check_substitute = ""
if getDeviceCount("accelerator"):
    print("Found available accelerator device")
    acc_run_substitute = " env SYCL_DEVICE_TYPE=ACC "
    acc_check_substitute = "| FileCheck %s"
    config.available_features.add('accelerator')
config.substitutions.append( ('%ACC_RUN_PLACEHOLDER',  acc_run_substitute) )
config.substitutions.append( ('%ACC_CHECK_PLACEHOLDER',  acc_check_substitute) )

path = config.environment['PATH']
path = os.path.pathsep.join((config.llvm_tools_dir, path))
config.environment['PATH'] = path

# Device AOT compilation tools aren't part of the SYCL project,
# so they need to be pre-installed on the machine
aot_tools = ["opencl-aot", "ocloc", "aoc"]
for aot_tool in aot_tools:
    if find_executable(aot_tool) is not None:
        print("Found AOT device compiler " + aot_tool)
        config.available_features.add(aot_tool)
    else:
        print("Could not find AOT device compiler " + aot_tool)

# Set timeout for test = 10 mins
try:
    import psutil
    lit_config.maxIndividualTestTime = 600
except ImportError:
    pass
