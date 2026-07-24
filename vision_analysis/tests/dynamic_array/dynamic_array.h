#ifndef DYNAMIC_ARRAY_H
#define DYNAMIC_ARRAY_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    int *data;
    size_t size;
    size_t capacity;
} DynamicArray;


// 创建并初始化一个空动态数组
int dynamic_array_init(DynamicArray *array);

// 释放这个动态数组申请的所有内存，使它不再占用堆内存
void dynamic_array_destroy(DynamicArray *array);

// 在末尾添加元素
int dynamic_array_push_back(DynamicArray *array, int value);

// 获取指定下标位置的元素
int dynamic_array_get(const DynamicArray *array, size_t index, int *value);

// 修改指定下标位置的元素
int dynamic_array_set(DynamicArray *array, size_t index, int value);

// 在指定下标位置插入一个整数
int dynamic_array_insert(DynamicArray *array, size_t index, int value);

// 删除指定下标位置的元素
int dynamic_array_erase(DynamicArray *array, size_t index);

// 获取动态数组当前的元素数量
size_t dynamic_array_size(const DynamicArray *array);

// 判断动态数组是否为空
int dynamic_array_empty(const DynamicArray *array);

// 清空动态数组中的所有元素
void dynamic_array_clear(DynamicArray *array);

#endif
