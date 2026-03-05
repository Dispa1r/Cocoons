# Cocoons（作茧）项目规格说明书

> 作茧，不是自缚，每一根丝的加持，都是为了保护！

## 1. 项目概述

Cocoons 是一个基于 LLVM 的 iOS 代码混淆工具。它以 LLVM Pass Plugin（动态库）的形式独立编译，通过 `-fpass-plugin` 加载到编译流程中，在编译期对代码进行混淆处理，从而增加逆向工程的难度，保护 iOS 应用的安全。

- **仓库地址**: https://github.com/sweetloser/Cocoons
- **当前版本**: v3.0.0（2026-02-26）
- **LLVM 版本**: 21.1.8
- **开源协议**: Apache 2.0

## 2. 功能清单

| 功能 | 状态 | 说明 |
|------|------|------|
| C/OC 字符串混淆 | ✅ 已实现 | 编译期加密字符串，运行时按需懒解密 |
| 指令替换 | ✅ 已实现 | 将简单算术指令替换为等价的复杂形式 |
| 控制流平坦化 | ⬜ 计划中 | 打乱函数控制流结构 |
| 反调试 | ⬜ 计划中 | 检测并阻止调试器附加 |

## 3. 技术栈

- **语言**: C++
- **编译器基础设施**: LLVM 21.1.8
- **构建系统**: CMake + Ninja
- **目标平台**: iOS（AArch64、ARM、X86）
- **集成方式**: Pass Plugin 动态库（`.dylib`），通过 `-fpass-plugin` 加载

## 4. 项目结构

```
Cocoons/
├── llvm/
│   ├── lib/Transforms/Cocoons/          # 核心混淆 Pass 实现
│   │   ├── CocoonsPasses.cpp            # Pass Plugin 入口（52 行）
│   │   ├── StringObfuscationPass.cpp    # 字符串混淆 Pass（593 行）
│   │   ├── SubstitutionPass.cpp         # 指令替换 Pass（75 行）
│   │   └── CMakeLists.txt               # 构建配置（19 行）
│   ├── include/llvm/Transforms/Cocoons/ # 头文件
│   │   ├── StringObfuscationPass.h      # 字符串混淆接口（58 行）
│   │   └── SubstitutionPass.h           # 指令替换接口（19 行）
│   ├── lib/Passes/                      # LLVM Pass 管线
│   │   ├── PassBuilderPipelines.cpp     # 已移除 Cocoons 硬编码注册
│   │   └── CMakeLists.txt               # 已移除 Cocoons 链接依赖
│   └── tools/                           # LLVM 工具链（clang, lld 等）
├── clang/                               # Clang 编译器前端
├── lld/                                 # LLVM 链接器
├── compiler-rt/                         # 编译器运行时库
├── tests/
│   └── test_string_obf_v2.m             # 字符串混淆验证 demo（67 行）
├── build.sh                             # 构建脚本
├── spec.md                              # 本文档
├── README.md                            # 项目文档
├── Documentation/
│   └── RELEASE_NOTE_CN.md               # 更新日志
└── LICENSE.TXT                          # Apache 2.0 许可证
```

## 5. 架构：Pass Plugin 模式

### 5.1 为什么用 Pass Plugin

v1/v2 时代 Cocoons 作为 LLVM 组件库（`add_llvm_component_library(LLVMCocoons ...)`）静态链接到 clang 中，每次修改 Pass 代码都需要重新编译整个 clang（耗时数十分钟）。

v3 改为 Pass Plugin 动态库（`add_llvm_pass_plugin(CocoonsPasses ...)`），只需编译 Pass 本身（几秒钟），通过 `-fpass-plugin=CocoonsPasses.dylib` 动态加载。

### 5.2 Plugin 入口（CocoonsPasses.cpp）

```cpp
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {LLVM_PLUGIN_API_VERSION, "CocoonsPasses", "3.0.0", registerCocoonsPasses};
}
```

`registerCocoonsPasses` 注册两类回调：

1. **OptimizerLastEPCallback** — 在优化管线末尾自动注入 Pass（需 `-O1` 及以上）
2. **PipelineParsingCallback** — 支持 `opt` 工具通过名字加载（`cocoons-str-obf`、`cocoons-sub`）

### 5.3 CMake 构建配置

```cmake
add_llvm_pass_plugin(CocoonsPasses
  CocoonsPasses.cpp
  StringObfuscationPass.cpp
  SubstitutionPass.cpp
  ...
)

# macOS 上用 -undefined dynamic_lookup 让 dylib 在加载时从宿主进程解析 LLVM 符号
if(APPLE)
  target_link_options(CocoonsPasses PRIVATE -undefined dynamic_lookup)
endif()
```

关键点：不显式链接 `LLVMCore`/`LLVMSupport` 等静态库，避免 `cl::opt` 重复注册导致的 `Option registered more than once` 崩溃。dylib 在运行时从宿主进程（clang）解析 LLVM 符号。

### 5.4 开关机制

由于 `-mllvm` 选项在 Pass Plugin dylib 加载之前就已被解析，`cl::opt` 变量来不及注册。因此 `isEnabled()` 同时支持两种方式：

| 方式 | 适用场景 | 示例 |
|------|----------|------|
| `cl::opt` | 静态链接到 clang | `-mllvm -cocoons-enable-str` |
| 环境变量 | Pass Plugin 动态加载 | `COCOONS_ENABLE_STR=1` |

```cpp
bool StringObfuscationPass::isEnabled() {
    if (EnableStrObf) return true;                          // cl::opt
    if (const char *Env = std::getenv("COCOONS_ENABLE_STR"))
        return std::string(Env) == "1";                     // 环境变量
    return false;
}
```

## 6. 核心模块详解

### 6.1 字符串混淆 Pass（StringObfuscationPass）

命名空间: `cocoons`
类型: Module Pass（`PassInfoMixin<StringObfuscationPass>`）
启用方式: `COCOONS_ENABLE_STR=1`（环境变量）或 `-mllvm -cocoons-enable-str`（静态链接）

#### 6.1.1 整体工作流程（v3）

```
run()
  ├── 1. 扫描 llvm.global.annotations → 收集 obfuscate 标记的全局变量
  ├── 2. createDecryptFunctions(M) → 生成两个解密函数（Mode A + Mode B）
  ├── 3. encryptRealData(M, GV) × N → 随机选模式，加密每个字符串
  └── 4. instrumentUseSites(M, Entries) → 在每个使用点前插入解密调用
```

#### 6.1.2 标记识别与变量解析

扫描 `llvm.global.annotations`，查找带有 `__attribute__((annotate("obfuscate")))` 注解的全局变量。`processVariable()` 递归遍历变量引用，定位实际的 i8 字节数组，支持：

- 直接字节数组（i8 序列）
- Objective-C `NSConstantString` 结构体（提取第 3 个成员）
- C 字符串指针引用
- 复杂常量表达式（GEP 操作）

#### 6.1.3 双模式加密（encryptRealData）

编译时为每个字符串随机选择两种加密模式之一（50/50）：

**Mode A — Key Array（密钥数组）**

- 为每个字节生成独立的随机 key（1-255）和随机 op（0=XOR, 1=ADD, 2=SUB）
- key 序列和 op 序列分别存储为 `[N x i8]` 类型的 internal GlobalVariable
- 优点：密钥完全随机，安全性最高
- 缺点：二进制体积增加 2× 字符串长度

**Mode B — Seed + PRNG（种子派生）**

- 为每个字符串生成一个 `uint32_t` 随机种子
- 解密时用 xorshift32 PRNG 从种子逐字节派生 key 和 op
- 优点：只需存储 4 字节种子，体积极小
- 缺点：PRNG 可被逆向推导，但需要先识别算法

**Per-byte 加密逻辑**（两种模式共用）：

```
编译时加密：                    运行时解密：
op=0: encrypted = plain ^ key   decrypted = encrypted ^ key
op=1: encrypted = plain + key   decrypted = encrypted - key
op=2: encrypted = plain - key   decrypted = encrypted + key
```

末尾 `\0` 不加密，保证 printf 兼容性。加密后字符串留在默认 `__DATA` 段，与普通数据混在一起。

#### 6.1.4 两个解密函数（createDecryptFunctions）

生成两个 IR 函数，均使用 `LinkOnceODR + Hidden` 链接类型（多模块安全）。

**`__cocoons_decrypt_a`（Mode A 专用）**

```
void __cocoons_decrypt_a(ptr addr, i32 len, ptr keys, ptr ops, ptr guard)
```

- `addr`: 加密字符串地址
- `len`: 字符串长度（含 `\0`）
- `keys`: per-byte key 数组地址
- `ops`: per-byte op 数组地址
- `guard`: per-string guard 变量地址

IR 伪代码：
```
entry:
    cmpxchg guard, 0 → 1 (acq_rel/monotonic)
    br was_zero → do_decrypt / exit
do_decrypt:
    dec_len = len - 1
    br (dec_len > 0) → loop / exit
loop:
    key = load keys[idx]
    op  = load ops[idx]
    byte = load addr[idx]
    switch op:
      0: result = byte ^ key    // XOR
      1: result = byte - key    // SUB（还原 ADD）
      2: result = byte + key    // ADD（还原 SUB）
    store result → addr[idx]
    idx++, br (idx < dec_len) → loop / exit
exit:
    ret void
```

**`__cocoons_decrypt_b`（Mode B 专用）**

```
void __cocoons_decrypt_b(ptr addr, i32 len, i32 seed, ptr guard)
```

- `addr`: 加密字符串地址
- `len`: 字符串长度（含 `\0`）
- `seed`: xorshift32 PRNG 种子
- `guard`: per-string guard 变量地址

IR 伪代码：
```
entry:
    cmpxchg guard, 0 → 1 (acq_rel/monotonic)
    br was_zero → do_decrypt / exit
do_decrypt:
    dec_len = len - 1
    br (dec_len > 0) → loop / exit
loop:
    state = phi [seed, do_decrypt], [next_state, store]
    // xorshift32 #1 → 生成 key
    s1 = state ^ (state << 13)
    s2 = s1 ^ (s1 >> 17)
    s3 = s2 ^ (s2 << 5)
    key = trunc(s3) to i8; if key==0 then key=1
    // xorshift32 #2 → 生成 op
    s4 = s3 ^ (s3 << 13)
    s5 = s4 ^ (s4 >> 17)
    s6 = s5 ^ (s5 << 5)
    op = s6 % 3
    // 解密（同 Mode A 的 switch）
    byte = load addr[idx]
    switch op → XOR/SUB/ADD
    store result → addr[idx]
    next_state = s6
    idx++, br (idx < dec_len) → loop / exit
exit:
    ret void
```

#### 6.1.5 使用点插桩（instrumentUseSites）

递归遍历每个加密字符串的 user 链（worklist 算法），收集所有 Instruction 级别的使用者：

```
GV → ConstantExpr → GlobalVariable → ... → Instruction
```

跳过规则：
- `llvm.global.annotations`、`llvm.used`、`llvm.compiler.used` 等元数据
- 解密函数自身内部的指令（`DecryptFuncA` / `DecryptFuncB`）

根据 `Entry.UsePRNG` 选择调用哪个解密函数：
- Mode A: `call @__cocoons_decrypt_a(addr, len, keys, ops, guard)`
- Mode B: `call @__cocoons_decrypt_b(addr, len, seed, guard)`

PHI 节点特殊处理：在对应前驱块的 terminator 前插入 call（不能在 PHI 节点前插入普通指令）。

#### 6.1.6 关键实现细节

- 加密后字符串标记为非常量（`setConstant(false)`），清除原有 section（`setSection("")`），留在默认 `__DATA` 段
- 不使用自定义 section（`__obf_strings`、`__cocoons_obs` 均已移除）
- 两个解密函数均使用 `LinkOnceODR` 链接类型和 `Hidden` 可见性，支持多模块场景
- 使用 `cmpxchg` 原子操作（`AcquireRelease` / `Monotonic`）保证线程安全
- 不做编译期去重，每个 use site 前都插入 call，运行时由 guard 保证幂等
- 每次编译产生不同的加密结果（`std::random_device` 驱动），增加逆向难度
- xorshift32 PRNG 编译时（C++ 函数）和运行时（IR 生成）使用完全相同的算法，保证一致性

#### 6.1.7 数据结构

```cpp
struct EncryptedStringInfo {
    GlobalVariable *GV;                    // 加密后的字符串全局变量
    uint32_t Len;                          // 字符串长度（含 \0）
    GlobalVariable *GuardGV;               // per-string guard 变量（i8, init 0）
    bool UsePRNG;                          // true = Mode B, false = Mode A
    GlobalVariable *KeyArrayGV = nullptr;  // Mode A: [N x i8] per-byte key 数组
    GlobalVariable *OpArrayGV  = nullptr;  // Mode A: [N x i8] per-byte op 数组
    uint32_t Seed = 0;                     // Mode B: PRNG seed
};
```

### 6.2 指令替换 Pass（SubstitutionPass）

命名空间: `cocoons`
类型: Function Pass（`PassInfoMixin<SubstitutionPass>`）
启用方式: `COCOONS_ENABLE_SUB=1`（环境变量）或 `-mllvm -cocoons-enable-sub`（静态链接）

#### 工作流程

1. **扫描**: 遍历函数中所有基本块，收集全部 `BinaryOperator` 指令
2. **替换**: 对整数加法指令执行等价变换：
   - 原始: `a + b`
   - 变换: `(a ^ b) + 2 * (a & b)`
3. **清理**: 移除已无引用的原始指令

#### 数学等价性证明

`a + b = (a ^ b) + 2 * (a & b)` 成立的原因：
- `a ^ b` 计算不进位加法（每一位独立相加，不考虑进位）
- `a & b` 找出所有需要进位的位
- `2 * (a & b)` 将进位左移一位
- 两者相加即为完整的加法结果

## 7. 构建与使用

### 7.1 构建 Plugin

```bash
cd Cocoons/build

# 配置（只需一次）
cmake -G Ninja ../llvm \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang" \
  -DLLVM_TARGETS_TO_BUILD="AArch64;ARM;X86"

# 编译 Plugin（几秒钟）
ninja CocoonsPasses

# 产物：build/lib/CocoonsPasses.dylib（~95KB）
```

### 7.2 使用 Plugin 编译

```bash
# 启用字符串混淆
COCOONS_ENABLE_STR=1 clang -O1 \
  -isysroot $(xcrun --show-sdk-path) \
  -fpass-plugin=/path/to/CocoonsPasses.dylib \
  -framework Foundation \
  source.m -o output

# 启用指令替换
COCOONS_ENABLE_SUB=1 clang -O1 \
  -fpass-plugin=/path/to/CocoonsPasses.dylib \
  source.m -o output

# 同时启用两者
COCOONS_ENABLE_STR=1 COCOONS_ENABLE_SUB=1 clang -O1 \
  -fpass-plugin=/path/to/CocoonsPasses.dylib \
  -framework Foundation \
  source.m -o output
```

注意：需要 `-O1` 及以上优化级别，`-O0` 时 Pass 不会运行。

### 7.3 标记需要混淆的字符串

```objective-c
__attribute__((annotate("obfuscate")))
const char c_array[] = "C Array Hello World!";

__attribute__((annotate("obfuscate")))
const char *c_string = "C String Hello World!";

__attribute__((annotate("obfuscate")))
NSString *ocString = @"OC String Hello World!";
```

未标记的字符串不受影响，保持明文。

### 7.4 验证混淆效果

```bash
# 验证加密字符串不可读
strings output | grep "Hello"  # 应无输出

# 验证无自定义段
otool -l output | grep __obf_strings    # 应无输出
otool -l output | grep __cocoons        # 应无输出

# 验证运行正确
./output  # 所有字符串正常输出

# 验证随机性（两次编译产生不同二进制）
md5 output1 output2  # MD5 应不同
```

### 7.5 构建完整 Toolchain（可选）

如果需要安装为 Xcode Toolchain（包含 clang + plugin）：

```bash
cmake -G Ninja ../llvm \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_CREATE_XCODE_TOOLCHAIN=ON \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_ENABLE_RUNTIMES="compiler-rt" \
  -DLLVM_TARGETS_TO_BUILD="AArch64;ARM;X86" \
  -DAPPLE_CONFIG_SANS_IPHONEOS=ON

ninja install-xcode-toolchain
# 安装至 ~/Library/Developer
```

## 8. 源文件清单

| 文件 | 类型 | 行数 | 说明 |
|------|------|------|------|
| `llvm/lib/Transforms/Cocoons/CocoonsPasses.cpp` | 实现 | 52 | Pass Plugin 入口，注册回调 |
| `llvm/lib/Transforms/Cocoons/StringObfuscationPass.cpp` | 实现 | 593 | 字符串混淆核心逻辑（v3 双模式） |
| `llvm/lib/Transforms/Cocoons/SubstitutionPass.cpp` | 实现 | 75 | 指令替换核心逻辑 |
| `llvm/include/llvm/Transforms/Cocoons/StringObfuscationPass.h` | 头文件 | 58 | 字符串混淆 Pass 接口定义 |
| `llvm/include/llvm/Transforms/Cocoons/SubstitutionPass.h` | 头文件 | 19 | 指令替换 Pass 接口定义 |
| `llvm/lib/Transforms/Cocoons/CMakeLists.txt` | 构建 | 19 | Pass Plugin 构建配置 |
| `tests/test_string_obf_v2.m` | 测试 | 67 | 字符串混淆验证 demo |
| `build.sh` | 脚本 | 50 | 编译与安装脚本 |

## 9. 版本历史

| 版本 | 发布时间 | 变更内容 |
|------|----------|----------|
| v1.0.0 | 2026-02-09 | 初始版本：C/OC 字符串混淆（启动时批量解密）、加法指令替换 |
| v1.0.1 | 2026-02-10 | 修复字符串混淆在多 module 下的 bug |
| v2.0.0 | 2026-02-25 | 字符串混淆升级为懒解密：按需解密、cmpxchg 线程安全、加密字符串留在默认 `__DATA` 段 |
| v3.0.0 | 2026-02-26 | 每字节独立密钥 + 随机加密方式（XOR/ADD/SUB）+ 双模式（key 数组 / seed+PRNG）；改为 Pass Plugin 动态库架构 |
