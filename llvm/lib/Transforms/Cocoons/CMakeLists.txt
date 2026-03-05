# Cocoons Pass Plugin（动态库）
# 不显式链接 LLVMCore/LLVMSupport 等静态库，
# 运行时从宿主进程（clang）解析符号，避免 cl::opt 重复注册。
add_llvm_pass_plugin(CocoonsPasses
  CocoonsPasses.cpp
  StringObfuscationPass.cpp
  SubstitutionPass.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/Cocoons

  DEPENDS
  intrinsics_gen
)

# macOS 上用 -undefined dynamic_lookup 让 dylib 在加载时从宿主进程解析 LLVM 符号
if(APPLE)
  target_link_options(CocoonsPasses PRIVATE -undefined dynamic_lookup)
endif()
