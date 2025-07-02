git clone https://github.com/llvm/llvm-project --depth 1
cd llvm-project
set MAX_PARALLEL_COMPILE_JOBS=64
set MAX_PARALLEL_LINK_JOBS=64
call C:\\BuildTools\\Common7\\Tools\\VsDevCmd.bat -arch=amd64 -host_arch=amd64
bash .ci/monolithic-windows.sh "llvm clang lld lldb" check-lldb
