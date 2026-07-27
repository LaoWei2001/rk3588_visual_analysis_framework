#include "dynamic_array.h"

// typedef struct
// {
//     int *data;
//     size_t size;
//     size_t capacity;
// } DynamicArray;

// 创建并初始化一个空动态数组
int dynamic_array_init(DynamicArray *array)
{
    if (!array)
    {
        printf("指针是空指针，初始化失败\n");
        return 1;
    }
    // 数据地址
    array->data = NULL;
    // 有效元素个数
    array->size = 0;
    // 最大容量
    array->capacity = 0;
    return 0;
}

// 释放这个动态数组申请的所有内存，使它不再占用堆内存
void dynamic_array_destroy(DynamicArray *array)
{
    if (!array)
    {
        printf("销毁失败, 传入的为空指针\n");
        return;
    }
    free(array->data);
    array->data = NULL;
    array->size = 0;
    array->capacity = 0;
}

// 在末尾添加元素
int dynamic_array_push_back(DynamicArray *array, int value)
{
    if (!array)
    {
        printf("指针为空,检查数据初始化\n");
        return 1;
    }
    if (array->size > array->capacity)
    {
        printf("数组内部状态错误");
        return 1;
    }
    // 空间不足时
    if (array->capacity == array->size)
    {
        size_t new_capacity = 2 * (array->capacity + 1);
        int *data_new = realloc(array->data, new_capacity * sizeof(*array->data));
        if (!data_new)
        {
            printf("数组扩容失败\n");
            return 1;
        }
        array->data = data_new;
        array->capacity = new_capacity;
    }
    if (!array->data)
    {
        printf("data指针为空,检查数据初始化\n");
        return 1;
    }
    (array->data)[array->size] = value;
    array->size++;
    return 0;
}

// 获取指定下标位置的元素
int dynamic_array_get(const DynamicArray *array, size_t index, int *value)
{
    if (!array || !array->data)
    {
        printf("查询失败,请检查数组状态\n");
        return 1;
    }
    if (index >= array->size)
    {
        printf("查询失败,下标越界\n");
        return 1;
    }
    if (!value)
    {
        printf("查询失败,未指定位置存储查询结果\n");
        return 1;
    }
    *value = (array->data)[index];
    return 0;
}

// 修改指定下标位置的元素
int dynamic_array_set(DynamicArray *array, size_t index, int value)
{
    if (!array || !array->data)
    {
        printf("修改失败,请检查数组状态\n");
        return 1;
    }
    if (index >= array->size)
    {
        printf("修改失败,下标越界\n");
        return 1;
    }
    (array->data)[index] = value;
    return 0;
}

// 在指定下标位置插入一个整数, 后面的元素整体后移
int dynamic_array_insert(DynamicArray *array, size_t index, int value)
{
    if (!array)
    {
        printf("指针是空指针，插入失败\n");
        return 1;
    }
    // 下标检测
    if (index > array->size)
    {
        printf("插入失败,下标越界\n");
        return 1;
    }
    if (dynamic_array_push_back(array, value))
    {
        return 1;
    }
    size_t nums = array->size - index - 1;
    memmove(&array->data[index + 1], &array->data[index], nums * sizeof(*array->data));
    array->data[index] = value;
    return 0;
}

// 删除指定下标位置的元素
int dynamic_array_erase(DynamicArray *array, size_t index)
{
    if (!array)
    {
        printf("指针是空指针，删除失败\n");
        return 1;
    }
    // 下标检测
    if (index >= array->size)
    {
        printf("删除失败,下标越界\n");
        return 1;
    }
    size_t nums = array->size - index - 1;
    memmove(&array->data[index], &array->data[index + 1], nums * sizeof(*array->data));
    array->size--;
    return 0;
}

// 获取动态数组当前的元素数量
size_t dynamic_array_size(const DynamicArray *array)
{
    if (!array)
    {
        printf("指针是空指针，查询失败\n");
        return 0;
    }
    return array->size;
}

// 判断动态数组是否为空
int dynamic_array_empty(const DynamicArray *array)
{
    if (!array)
    {
        printf("指针是空指针，查询失败\n");
        return -1;
    }
    return (array->size == 0) ? 1 : 0;
}

// 清空动态数组中的所有元素
void dynamic_array_clear(DynamicArray *array)
{
    if (!array)
    {
        printf("指针是空指针，清理失败\n");
        return;
    }
    array->size = 0;
}

int copy_element(void *dst, const void *src, size_t element_size)
{
    
}
