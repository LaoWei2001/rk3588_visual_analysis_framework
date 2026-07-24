#include "dynamic_array.h"

#include <stddef.h>
#include <stdio.h>

static int g_total_tests = 0;
static int g_passed_tests = 0;

static void check(int condition, const char *description)
{
    g_total_tests++;

    if (condition)
    {
        g_passed_tests++;
        printf("[PASS] %s\n", description);
    }
    else
    {
        printf("[FAIL] %s\n", description);
    }
}

static void print_array(const DynamicArray *array)
{
    if (array == NULL)
    {
        printf("NULL\n");
        return;
    }

    printf("[");
    for (size_t i = 0; i < array->size; i++)
    {
        printf("%d", array->data[i]);
        if (i + 1 < array->size)
        {
            printf(", ");
        }
    }
    printf("] size=%zu capacity=%zu\n", array->size, array->capacity);
}

static void check_contents(
    const DynamicArray *array,
    const int *expected,
    size_t expected_size,
    const char *description)
{
    int correct = 1;

    if (array == NULL || array->size != expected_size)
    {
        correct = 0;
    }
    else
    {
        for (size_t i = 0; i < expected_size; i++)
        {
            if (array->data[i] != expected[i])
            {
                correct = 0;
                break;
            }
        }
    }

    check(correct, description);

    if (!correct)
    {
        printf("       实际结果: ");
        print_array(array);

        printf("       期望结果: [");
        for (size_t i = 0; i < expected_size; i++)
        {
            printf("%d", expected[i]);
            if (i + 1 < expected_size)
            {
                printf(", ");
            }
        }
        printf("] size=%zu\n", expected_size);
    }
}

static void test_null_arguments(void)
{
    int value = 0;

    printf("\n========== 空指针参数测试 ==========\n");

    check(dynamic_array_init(NULL) != 0,
          "dynamic_array_init(NULL) 应失败");

    check(dynamic_array_push_back(NULL, 10) != 0,
          "dynamic_array_push_back(NULL, ...) 应失败");

    check(dynamic_array_get(NULL, 0, &value) != 0,
          "dynamic_array_get(NULL, ...) 应失败");

    check(dynamic_array_set(NULL, 0, 10) != 0,
          "dynamic_array_set(NULL, ...) 应失败");

    check(dynamic_array_insert(NULL, 0, 10) != 0,
          "dynamic_array_insert(NULL, ...) 应失败");

    check(dynamic_array_erase(NULL, 0) != 0,
          "dynamic_array_erase(NULL, ...) 应失败");

    check(dynamic_array_size(NULL) == 0,
          "dynamic_array_size(NULL) 按当前接口应返回 0");

    check(dynamic_array_empty(NULL) == -1,
          "dynamic_array_empty(NULL) 按当前接口应返回 -1");

    dynamic_array_clear(NULL);
    check(1, "dynamic_array_clear(NULL) 不应崩溃");

    dynamic_array_destroy(NULL);
    check(1, "dynamic_array_destroy(NULL) 不应崩溃");
}

static void test_normal_operations(void)
{
    DynamicArray array;
    int value = 0;

    printf("\n========== 正常功能与边界测试 ==========\n");

    check(dynamic_array_init(&array) == 0,
          "初始化应成功");

    check(array.data == NULL && array.size == 0 && array.capacity == 0,
          "初始化后状态应为 data=NULL、size=0、capacity=0");

    check(dynamic_array_size(&array) == 0,
          "新数组的 size 应为 0");

    check(dynamic_array_empty(&array) == 1,
          "新数组应为空");

    check(dynamic_array_get(&array, 0, &value) != 0,
          "空数组 get(0) 应失败");

    check(dynamic_array_erase(&array, 0) != 0,
          "空数组 erase(0) 应失败");

    check(dynamic_array_insert(&array, 1, 99) != 0,
          "空数组只能在下标 0 插入，在下标 1 插入应失败");

    check(dynamic_array_push_back(&array, 10) == 0,
          "push_back(10) 应成功");
    check(dynamic_array_push_back(&array, 20) == 0,
          "push_back(20) 应成功");
    check(dynamic_array_push_back(&array, 30) == 0,
          "push_back(30) 应成功并可能触发扩容");
    check(dynamic_array_push_back(&array, 40) == 0,
          "push_back(40) 应成功");

    {
        const int expected[] = {10, 20, 30, 40};
        check_contents(&array, expected, 4,
                       "连续 push_back 后内容应正确");
    }

    check(array.capacity >= array.size,
          "任何时候 capacity 都不应小于 size");

    check(dynamic_array_size(&array) == 4,
          "push_back 4 个元素后 size 应为 4");

    check(dynamic_array_empty(&array) == 0,
          "包含元素的数组不应为空");

    check(dynamic_array_get(&array, 2, &value) == 0 && value == 30,
          "get(2) 应取得 30");

    check(dynamic_array_get(&array, array.size, &value) != 0,
          "get(size) 应越界失败");

    check(dynamic_array_get(&array, 0, NULL) != 0,
          "get 的输出指针为 NULL 时应失败");

    check(dynamic_array_set(&array, 1, 25) == 0,
          "set(1, 25) 应成功");

    check(dynamic_array_get(&array, 1, &value) == 0 && value == 25,
          "set 后 get(1) 应得到 25");

    check(dynamic_array_set(&array, array.size, 100) != 0,
          "set(size, ...) 应越界失败");

    check(dynamic_array_insert(&array, 0, 5) == 0,
          "在开头插入 5 应成功");

    {
        const int expected[] = {5, 10, 25, 30, 40};
        check_contents(&array, expected, 5,
                       "开头插入后的内容应正确");
    }

    check(dynamic_array_insert(&array, 3, 27) == 0,
          "在中间下标 3 插入 27 应成功");

    {
        const int expected[] = {5, 10, 25, 27, 30, 40};
        check_contents(&array, expected, 6,
                       "中间插入后的内容应正确");
    }

    {
        size_t old_size = array.size;
        check(dynamic_array_insert(&array, array.size, 50) == 0,
              "在下标 size 处插入应等价于末尾添加");
        check(array.size == old_size + 1,
              "末尾插入后 size 应增加 1");
    }

    {
        const int expected[] = {5, 10, 25, 27, 30, 40, 50};
        check_contents(&array, expected, 7,
                       "末尾插入后的内容应正确");
    }

    {
        size_t old_size = array.size;
        check(dynamic_array_insert(&array, array.size + 1, 999) != 0,
              "在 size+1 处插入应失败");
        check(array.size == old_size,
              "非法插入不应改变 size");
    }

    check(dynamic_array_erase(&array, 0) == 0,
          "删除第一个元素应成功");

    {
        const int expected[] = {10, 25, 27, 30, 40, 50};
        check_contents(&array, expected, 6,
                       "删除第一个元素后的内容应正确");
    }

    check(dynamic_array_erase(&array, 2) == 0,
          "删除中间下标 2 的元素应成功");

    {
        const int expected[] = {10, 25, 30, 40, 50};
        check_contents(&array, expected, 5,
                       "删除中间元素后的内容应正确");
    }

    check(dynamic_array_erase(&array, array.size - 1) == 0,
          "删除最后一个元素应成功");

    {
        const int expected[] = {10, 25, 30, 40};
        check_contents(&array, expected, 4,
                       "删除最后一个元素后的内容应正确");
    }

    {
        size_t old_size = array.size;
        check(dynamic_array_erase(&array, array.size) != 0,
              "erase(size) 应越界失败");
        check(array.size == old_size,
              "非法删除不应改变 size");
    }

    {
        int *old_data = array.data;
        size_t old_capacity = array.capacity;

        dynamic_array_clear(&array);

        check(array.size == 0,
              "clear 后 size 应为 0");
        check(array.capacity == old_capacity,
              "clear 后应保留 capacity");
        check(array.data == old_data,
              "clear 后应保留已申请的内存地址");
        check(dynamic_array_empty(&array) == 1,
              "clear 后数组应为空");
    }

    check(dynamic_array_get(&array, 0, &value) != 0,
          "clear 后 get(0) 应失败");

    check(dynamic_array_erase(&array, 0) != 0,
          "clear 后 erase(0) 应失败");

    check(dynamic_array_push_back(&array, 123) == 0,
          "clear 后应能重新 push_back");

    {
        const int expected[] = {123};
        check_contents(&array, expected, 1,
                       "clear 后重新添加的内容应正确");
    }

    dynamic_array_destroy(&array);

    check(array.data == NULL && array.size == 0 && array.capacity == 0,
          "destroy 后应恢复为 NULL、0、0");

    dynamic_array_destroy(&array);
    check(array.data == NULL && array.size == 0 && array.capacity == 0,
          "重复 destroy 不应崩溃，状态仍应为 NULL、0、0");

    check(dynamic_array_init(&array) == 0,
          "destroy 后重新 init 应成功");

    dynamic_array_destroy(&array);
}

int main(void)
{
    test_normal_operations();
    test_null_arguments();

    printf("\n========== 测试汇总 ==========\n");
    printf("通过：%d\n", g_passed_tests);
    printf("总数：%d\n", g_total_tests);
    printf("失败：%d\n", g_total_tests - g_passed_tests);

    if (g_passed_tests == g_total_tests)
    {
        printf("结果：全部测试通过。\n");
        return 0;
    }

    printf("结果：存在测试失败，请检查上方 [FAIL] 项。\n");
    return 1;
}
