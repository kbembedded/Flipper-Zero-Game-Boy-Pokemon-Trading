#ifndef TEST_H
#define TEST_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int test_failures = 0;
static int test_count = 0;

#define TEST(name) static void name(void)

#define RUN(name) \
    do { \
        int before_ = test_failures; \
        name(); \
        test_count++; \
        if(test_failures != before_) printf("  FAIL %s\n", #name); \
    } while(0)

#define ASSERT_TRUE(x) \
    do { \
        if(!(x)) { \
            printf("%s:%d: ASSERT_TRUE(%s) failed\n", __FILE__, __LINE__, #x); \
            test_failures++; \
            return; \
        } \
    } while(0)

#define ASSERT_EQ(a, b) \
    do { \
        long long a_ = (long long)(a); \
        long long b_ = (long long)(b); \
        if(a_ != b_) { \
            printf( \
                "%s:%d: ASSERT_EQ(%s, %s) failed: 0x%llx != 0x%llx\n", \
                __FILE__, \
                __LINE__, \
                #a, \
                #b, \
                (unsigned long long)a_, \
                (unsigned long long)b_); \
            test_failures++; \
            return; \
        } \
    } while(0)

#define ASSERT_MEM_EQ(a, b, n) \
    do { \
        if(memcmp((a), (b), (n)) != 0) { \
            printf("%s:%d: ASSERT_MEM_EQ(%s, %s, %s) failed\n", __FILE__, __LINE__, #a, #b, #n); \
            test_failures++; \
            return; \
        } \
    } while(0)

#define ASSERT_STR_EQ(a, b) \
    do { \
        if(strcmp((a), (b)) != 0) { \
            printf( \
                "%s:%d: ASSERT_STR_EQ failed: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
            test_failures++; \
            return; \
        } \
    } while(0)

#define TEST_EXIT() \
    do { \
        printf("%s: %d passed, %d failed\n", TEST_NAME, test_count - test_failures, test_failures); \
        return test_failures ? 1 : 0; \
    } while(0)

#endif
