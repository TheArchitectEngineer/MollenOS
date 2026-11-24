## The docker method

Fully prepared docker images with a toolchain installed are provided from valios/vali-toolchain. This repository
comes with a Dockerfile that you can use to build the OS with. You can also include the userspace
by downloading the build .tar file from the userspace repository and keep it in
the same folder as you run the docker command. Then docker will include it when you build an image file.

Building the OS with no artifacts
```
docker build --build-arg ARCH={i386,amd64} --file build.Dockerfile .
```

Building an .img file locally with docker and export it to host:
```
DOCKER_BUILDKIT=1 docker build --build-arg ARCH={i386,amd64} --output type=local,dest=build --file nightly.Dockerfile .
```

## The DIY method

Before you set up anything you must set up environmental variables that are used by
the project. This is only true if you do not do development with a docker container.

| Variable              | Required |                        Description                         |
|-----------------------|:---------|:----------------------------------------------------------:|
| VALICC                | Yes      | Points to the Vali cross-compiler installation root. |
| VALI_APPLICATION_PATH | No\*     | Points to where the Vali applications/libraries are built. |

\* Can be supplied to include built applications in the kernel image

### Building the toolchain

Toolchain scripts are located [here](https://github.com/Meulengracht/vali-toolchain). You should run the scripts in this order:
- depends.sh
- checkout.sh
- build-cross.sh

If you are not on Linux, you should download the llvm-project git and build it yourself using the cmake command
```
cmake -S llvm -B build -G "Unix Makefiles" 
   -DLLVM_TEMPORARILY_ALLOW_OLD_TOOLCHAIN=True
   -DLLVM_ENABLE_PROJECTS='clang;clang-tools-extra;libcxx;libcxxabi;libunwind;lldb;compiler-rt;lld'
   -DCMAKE_BUILD_TYPE=Release
   -DLLVM_INCLUDE_TESTS=Off
   -DLLVM_INCLUDE_EXAMPLES=Off
   -DCMAKE_INSTALL_PREFIX=/path/to/install
   -DLLVM_DEFAULT_TARGET_TRIPLE=amd64-uml-vali
```

### Building the operating system

Use cmake to configure the project, and supply the options you'd like to customize the build
with.

```
mkdir build
cd build
cmake -DVALI_ARCH={i386,amd64} ..
make
```

The above should be enough to build the kernel, services and drivers.

On Windows, use Ninja explicitly from a developer PowerShell with CMake, Ninja,
and NASM available on PATH:

```powershell
$env:VALICC = 'C:\Users\the_m\ValiCC'
$env:VALI_ARCH = {i386, amd64, aarch64}
cmake -S . -B build/windows -G Ninja -DVALI_ARCH=amd64
cmake --build build/windows
```

Use a fresh build directory if an existing one selected MSVC or the Visual Studio
generator. The OS uses the Vali Clang cross-compiler; the separate host tools
(such as `mkvafs`, `lzss`, and `file2c`) use a Windows compiler and may use MSVC.
If NASM is outside PATH, pass `-DCMAKE_ASM_NASM_COMPILER=C:/path/to/nasm.exe`
when configuring.

### Building the userspace (optional)

Just building this repository is not enough for the full OS experience. You need to checkout the userspace
repository that contains C++ runtime and applications and build those as well. For this you need to have the
SDK/DDK installed (```make install``` - remember to set CMAKE_INSTALL_PREFIX when configuring) and
the appropriate environmental variables' setup. Then the command ```make install_{img,vmdk}``` will automatically
pull in all built applications and libraries into the OS image.

| Option        |                                  Description                                  |
|---------------|:-----------------------------------------------------------------------------:|
| VALI_SDK_PATH | The path to the installed SDK, usually the prefix Vali is installed with      |

Then follow the instructions located [here](https://github.com/Meulengracht/vali-userspace) to get the sources for the applications.

### Generating the OS image

Build targets for generating OS images from the build are provided in the cmake system. You
can easily generate .img or .vmdk (more formats to come as requested). The following command
will also automatically include any files located in your VALI_APPLICATION_PATH.

```
cd <build> # where you have your build folder
make install_img # to generate a .img file
make install_vmdk # to generate a .vmdk file
```

### UTF-8 decoding in libds

`mstr_next` in `librt/libds/mstring/unicode/mstr_unicode.c` reads one UTF-8
character from a string. `mstr_len_u8`, `mstr_new_u8`, `mstr_fmt` and the
other mstring functions that take a `char*` read their input through it. It
reads at most 4 bytes per character and never reads past the zero terminator.
A byte that does not start a valid sequence gives U+FFFD, and the index moves
past that one byte. At the end of the string it returns `MSTRING_EOS`
(0xFFFFFFFF) and leaves the index where it is. Run the suite from the
repository root:

```sh
python3 testing/libds_utf8_test.py --mutation-check
```

It needs Python 3 and host Clang with ASan, UBSan and the leak checker. It
builds the mstring sources with each input copied to a buffer of the exact
size, so a read past the terminator stops the test. The cases cover valid 1 to
4 byte characters, sequences cut off by the end of the string or by a byte
that is not a continuation byte, long runs of continuation bytes, overlong forms, surrogates and values
above U+10FFFF. They also check that `mstr_len_u8`, `mstr_u8_to_internal` and
`mstr_new_u8` agree with `mstr_next`. The suite is registered with the host
CTest build.
