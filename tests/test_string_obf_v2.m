// test_string_obf_v2.m
// 字符串混淆 v2（懒解密）验证测试
//
// 编译命令：
//   xcrun --toolchain org.llvm.21.1.8 clang -O1 -mllvm -cocoons-enable-str \
//       -framework Foundation test_string_obf_v2.m -o test_obf
//
// 验证步骤：
//   1. strings test_obf | grep "Hello from"       → 应无输出（已加密）
//   2. strings test_obf | grep "visible in binary" → 应有输出（未加密）
//   3. otool -l test_obf | grep __obf_strings      → 应无输出（无自定义段）
//   4. otool -l test_obf | grep __cocoons_obs       → 应无输出（无元数据段）
//   5. ./test_obf                                   → 所有字符串正常输出

#import <Foundation/Foundation.h>
#include <stdio.h>

// ---- 混淆字符串 ----

__attribute__((annotate("obfuscate")))
const char c_array[] = "Hello from C array!";

__attribute__((annotate("obfuscate")))
const char *c_string = "Hello from C pointer!";

__attribute__((annotate("obfuscate")))
NSString *ocString = @"Hello from OC NSString!";

// ---- 普通字符串（对照组，不加密）----

const char *plain_string = "This should be visible in binary";

// ---- 测试：同函数多次使用同一字符串 ----
void testMultipleUses(void) {
    printf("First use:  %s\n", c_array);
    printf("Second use: %s\n", c_array);
}

// ---- 测试：跨函数使用同一字符串 ----
void testCrossFunction(void) {
    printf("Cross-func: %s\n", c_array);
}

// ---- main ----
int main(int argc, const char *argv[]) {
    @autoreleasepool {
        printf("=== C array ===\n");
        printf("C array: %s\n", c_array);

        printf("=== C pointer ===\n");
        printf("C pointer: %s\n", c_string);

        printf("=== OC NSString ===\n");
        NSLog(@"OC string: %@", ocString);

        printf("=== Multiple uses ===\n");
        testMultipleUses();

        printf("=== Cross function ===\n");
        testCrossFunction();

        printf("=== Plain (not obfuscated) ===\n");
        printf("Plain: %s\n", plain_string);
    }
    return 0;
}
