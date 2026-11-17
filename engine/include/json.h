/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

/** @file json.h
 * @brief cJSON 接口；示例中的 root/array/item 表示已创建的有效节点，省略分配失败处理。
 * 新树用 cJSON_Delete 释放；Print 字符串用 cJSON_free 释放；Get 返回借用指针。
 */

#ifndef cJSON__h
#define cJSON__h

#ifdef __cplusplus
extern "C"
{
#endif

#if !defined(__WINDOWS__) && (defined(WIN32) || defined(WIN64) || defined(_MSC_VER) || defined(_WIN32))
#define __WINDOWS__
#endif

#ifdef __WINDOWS__

    /* When compiling for windows, we specify a specific calling convention to avoid issues where we are being called
    from a project with a different default calling convention.  For windows you have 3 define options:

    CJSON_HIDE_SYMBOLS - Define this in the case where you don't want to ever dllexport symbols
    CJSON_EXPORT_SYMBOLS - Define this on library build when you want to dllexport symbols (default)
    CJSON_IMPORT_SYMBOLS - Define this if you want to dllimport symbol

    For *nix builds that support visibility attribute, you can define similar behavior by

    setting default visibility to hidden by adding
    -fvisibility=hidden (for gcc)
    or
    -xldscope=hidden (for sun cc)
    to CFLAGS

    then using the CJSON_API_VISIBILITY flag to "export" the same symbols the way CJSON_EXPORT_SYMBOLS does

    */

#define CJSON_CDECL __cdecl
#define CJSON_STDCALL __stdcall

/* export symbols by default, this is necessary for copy pasting the C and header file */
#if !defined(CJSON_HIDE_SYMBOLS) && !defined(CJSON_IMPORT_SYMBOLS) && !defined(CJSON_EXPORT_SYMBOLS)
#define CJSON_EXPORT_SYMBOLS
#endif

#if defined(CJSON_HIDE_SYMBOLS)
#define CJSON_PUBLIC(type) type CJSON_STDCALL
#elif defined(CJSON_EXPORT_SYMBOLS)
#define CJSON_PUBLIC(type) __declspec(dllexport) type CJSON_STDCALL
#elif defined(CJSON_IMPORT_SYMBOLS)
#define CJSON_PUBLIC(type) __declspec(dllimport) type CJSON_STDCALL
#endif
#else /* !__WINDOWS__ */
#define CJSON_CDECL
#define CJSON_STDCALL

#if (defined(__GNUC__) || defined(__SUNPRO_CC) || defined(__SUNPRO_C)) && defined(CJSON_API_VISIBILITY)
#define CJSON_PUBLIC(type) __attribute__((visibility("default"))) type
#else
#define CJSON_PUBLIC(type) type
#endif
#endif

/* project version */
#define CJSON_VERSION_MAJOR 1
#define CJSON_VERSION_MINOR 7
#define CJSON_VERSION_PATCH 10

#include <stddef.h>

/* cJSON Types: */
#define cJSON_Invalid (0)
#define cJSON_False (1 << 0)
#define cJSON_True (1 << 1)
#define cJSON_NULL (1 << 2)
#define cJSON_Number (1 << 3)
#define cJSON_String (1 << 4)
#define cJSON_Array (1 << 5)
#define cJSON_Object (1 << 6)
#define cJSON_Raw (1 << 7) /* raw json */
#define cJSON_IsReference 256
#define cJSON_StringIsConst 512

    /* The cJSON structure: */
    typedef struct cJSON
    {
        /* next/prev allow you to walk array/object chains. Alternatively, use GetArraySize/GetArrayItem/GetObjectItem
         */
        struct cJSON *next;
        struct cJSON *prev;
        /* An array or object item will have a child pointer pointing to a chain of the items in the array/object. */
        struct cJSON *child;

        /* The type of the item, as above. */
        int type;

        /* The item's string, if type==cJSON_String  and type == cJSON_Raw */
        char *valuestring;
        /* writing to valueint is DEPRECATED, use cJSON_SetNumberValue instead */
        int valueint;
        /* The item's number, if type==cJSON_Number */
        double valuedouble;

        /* The item's name string, if this item is the child of, or is in the list of subitems of an object. */
        char *string;
    } cJSON;

    typedef struct cJSON_Hooks
    {
        /** @brief 自定义内存分配函数；需与 free_fn 配套，在任何 JSON 分配前注册。
         * 示例：
         * @code
         * cJSON_Hooks hooks = {malloc, free};
         * cJSON_InitHooks(&hooks);
         * @endcode
         */
        void *(CJSON_CDECL *malloc_fn)(size_t sz);
        /** @brief 自定义释放函数；必须能释放 malloc_fn 返回的内存，运行期间不要更换。
         * 示例：
         * @code
         * cJSON_Hooks hooks = {malloc, free};
         * cJSON_InitHooks(&hooks);
         * @endcode
         */
        void(CJSON_CDECL *free_fn)(void *ptr);
    } cJSON_Hooks;

    typedef int cJSON_bool;

/* Limits how deeply nested arrays/objects can be before cJSON rejects to parse them.
 * This is to prevent stack overflows. */
#ifndef CJSON_NESTING_LIMIT
#define CJSON_NESTING_LIMIT 1000
#endif

    /** @brief 返回内置 cJSON 版本字符串，静态存储，无需释放。
     * 示例：
     * @code
     * const char *version = cJSON_Version();
     * @endcode
     */
    CJSON_PUBLIC(const char *) cJSON_Version(void);

    /** @brief 设置全局分配/释放器；NULL 恢复默认 malloc/free，只能在多线程及 JSON 分配前调用。
     * 示例：
     * @code
     * cJSON_InitHooks(NULL); // 程序初始化阶段
     * @endcode
     */
    CJSON_PUBLIC(void) cJSON_InitHooks(cJSON_Hooks *hooks);

    /* Memory Management: the caller is always responsible to free the results from all variants of cJSON_Parse (with
     * cJSON_Delete) and cJSON_Print (with stdlib free, cJSON_Hooks.free_fn, or cJSON_free as appropriate). The
     * exception is cJSON_PrintPreallocated, where the caller has full responsibility of the buffer. */
    /** @brief 解析以零结尾的 JSON，失败返回 NULL；成功的树由调用方 cJSON_Delete。
     * @note 此接口允许合法 JSON 后还有文本；严格校验用 ParseWithOpts(...,1)。
     * 示例：
     * @code
     * cJSON *root = cJSON_Parse("{\"count\":3}");
     * cJSON_Delete(root);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_Parse(const char *value);
    /* ParseWithOpts allows you to require (and check) that the JSON is null terminated, and to retrieve the pointer to
     * the final byte parsed. */
    /** @brief 解析 JSON，可返回结束/错误位置；require_null_terminated=1 拒绝尾部非空白文本。
     * @return 新树或 NULL；end 借用输入文本，并发诊断优先使用此局部输出。
     * 示例：
     * @code
     * const char *end = NULL;
     * cJSON *root = cJSON_ParseWithOpts("{\"count\":3}", &end, 1);
     * cJSON_Delete(root);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *)
    cJSON_ParseWithOpts(const char *value, const char **return_parse_end, cJSON_bool require_null_terminated);

    /** @brief 将 JSON 转为带缩进的字符串；返回 NULL 表示失败，成功结果用 cJSON_free 释放。
     * 示例：
     * @code
     * char *text = cJSON_Print(root);
     * cJSON_free(text);
     * @endcode
     */
    CJSON_PUBLIC(char *) cJSON_Print(const cJSON *item);
    /** @brief 将 JSON 转为无额外空白的字符串；返回 NULL 表示失败，成功结果用 cJSON_free 释放。
     * 示例：
     * @code
     * char *text = cJSON_PrintUnformatted(root);
     * cJSON_free(text);
     * @endcode
     */
    CJSON_PUBLIC(char *) cJSON_PrintUnformatted(const cJSON *item);
    /** @brief 将 JSON 转为可预估缓冲区大小的字符串；返回 NULL 表示失败，成功结果用 cJSON_free 释放。
     * @param prebuffer 预估字节数；fmt=0 紧凑输出，非 0 带缩进。
     * 示例：
     * @code
     * char *text = cJSON_PrintBuffered(root, 256, 0);
     * cJSON_free(text);
     * @endcode
     */
    CJSON_PUBLIC(char *) cJSON_PrintBuffered(const cJSON *item, int prebuffer, cJSON_bool fmt);
    /* Render a cJSON entity to text using a buffer already allocated in memory with given length. Returns 1 on success
     * and 0 on failure. */
    /** @brief 写入调用方 buffer，不分配输出字符串；length 含结尾零，容量建议多留 5 字节。
     * @return 1 成功，0 容量不足或输入无效；format 非 0 时带缩进。
     * 示例：
     * @code
     * char text[256];
     * int ok = cJSON_PrintPreallocated(root, text, sizeof(text), 0);
     * @endcode
     */
    CJSON_PUBLIC(cJSON_bool)
    cJSON_PrintPreallocated(cJSON *item, char *buffer, const int length, const cJSON_bool format);
    /** @brief 递归释放节点及其拥有的子树；NULL 安全，不单独删除仍挂在父节点下的借用项。
     * 示例：
     * @code
     * cJSON *root = cJSON_CreateObject();
     * cJSON_Delete(root);
     * @endcode
     */
    CJSON_PUBLIC(void) cJSON_Delete(cJSON *c);

    /** @brief 返回数组/对象的子项数量；NULL 返回 0。
     * 示例：
     * @code
     * int count = cJSON_GetArraySize(array);
     * @endcode
     */
    CJSON_PUBLIC(int) cJSON_GetArraySize(const cJSON *array);
    /** @brief 按从 0 开始的序号借用数组子项，越界返回 NULL；不单独释放。
     * 示例：
     * @code
     * cJSON *first = cJSON_GetArrayItem(array, 0);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_GetArrayItem(const cJSON *array, int index);
    /** @brief 按不区分大小写的键借用首个对象成员，未找到返回 NULL；不单独释放。
     * 示例：
     * @code
     * cJSON *count = cJSON_GetObjectItem(root, "count");
     * if (cJSON_IsNumber(count))
     * {
     *     double n = count->valuedouble;
     *     (void)n;
     * }
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_GetObjectItem(const cJSON *const object, const char *const string);
    /** @brief 按区分大小写的键借用首个对象成员，未找到返回 NULL；不单独释放。
     * 示例：
     * @code
     * cJSON *count = cJSON_GetObjectItemCaseSensitive(root, "count");
     * if (cJSON_IsNumber(count))
     * {
     *     double n = count->valuedouble;
     *     (void)n;
     * }
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_GetObjectItemCaseSensitive(const cJSON *const object, const char *const string);
    /** @brief 检查对象是否存在键，不区分大小写；值为 JSON null 也算存在。
     * 示例：
     * @code
     * int present = cJSON_HasObjectItem(root, "count");
     * @endcode
     */
    CJSON_PUBLIC(cJSON_bool) cJSON_HasObjectItem(const cJSON *object, const char *string);
    /** @brief 取得最近一次解析的错误位置，成功后为 NULL；指针借用输入，不适合并发诊断。
     * 示例：
     * @code
     * const char *error = cJSON_GetErrorPtr(); // 仅在同线程解析失败后使用
     * @endcode
     */
    CJSON_PUBLIC(const char *) cJSON_GetErrorPtr(void);

    /** @brief 借用字符串节点的文本；NULL 或非字符串返回 NULL，勿释放返回值。
     * 示例：
     * @code
     * const char *text =
     *     cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "label"));
     * @endcode
     */
    CJSON_PUBLIC(char *) cJSON_GetStringValue(cJSON *item);

    /** @brief 判断节点是否为无效节点类型（不是 NULL 指针）；返回 1/0，NULL 指针返回 0。
     * 示例：
     * @code
     * int matches = cJSON_IsInvalid(item);
     * @endcode
     */
    CJSON_PUBLIC(cJSON_bool) cJSON_IsInvalid(const cJSON *const item);
    /** @brief 判断节点是否为JSON false；返回 1/0，NULL 指针返回 0。
     * 示例：
     * @code
     * int matches = cJSON_IsFalse(item);
     * @endcode
     */
    CJSON_PUBLIC(cJSON_bool) cJSON_IsFalse(const cJSON *const item);
    /** @brief 判断节点是否为JSON true；返回 1/0，NULL 指针返回 0。
     * 示例：
     * @code
     * int matches = cJSON_IsTrue(item);
     * @endcode
     */
    CJSON_PUBLIC(cJSON_bool) cJSON_IsTrue(const cJSON *const item);
    /** @brief 判断节点是否为JSON 布尔值；返回 1/0，NULL 指针返回 0。
     * 示例：
     * @code
     * int matches = cJSON_IsBool(item);
     * @endcode
     */
    CJSON_PUBLIC(cJSON_bool) cJSON_IsBool(const cJSON *const item);
    /** @brief 判断节点是否为JSON null；返回 1/0，NULL 指针返回 0。
     * 示例：
     * @code
     * int matches = cJSON_IsNull(item);
     * @endcode
     */
    CJSON_PUBLIC(cJSON_bool) cJSON_IsNull(const cJSON *const item);
    /** @brief 判断节点是否为JSON number；返回 1/0，NULL 指针返回 0。
     * 示例：
     * @code
     * int matches = cJSON_IsNumber(item);
     * @endcode
     */
    CJSON_PUBLIC(cJSON_bool) cJSON_IsNumber(const cJSON *const item);
    /** @brief 判断节点是否为JSON string；返回 1/0，NULL 指针返回 0。
     * 示例：
     * @code
     * int matches = cJSON_IsString(item);
     * @endcode
     */
    CJSON_PUBLIC(cJSON_bool) cJSON_IsString(const cJSON *const item);
    /** @brief 判断节点是否为JSON array；返回 1/0，NULL 指针返回 0。
     * 示例：
     * @code
     * int matches = cJSON_IsArray(item);
     * @endcode
     */
    CJSON_PUBLIC(cJSON_bool) cJSON_IsArray(const cJSON *const item);
    /** @brief 判断节点是否为JSON object；返回 1/0，NULL 指针返回 0。
     * 示例：
     * @code
     * int matches = cJSON_IsObject(item);
     * @endcode
     */
    CJSON_PUBLIC(cJSON_bool) cJSON_IsObject(const cJSON *const item);
    /** @brief 判断节点是否为未经解析的 JSON raw 文本；返回 1/0，NULL 指针返回 0。
     * 示例：
     * @code
     * int matches = cJSON_IsRaw(item);
     * @endcode
     */
    CJSON_PUBLIC(cJSON_bool) cJSON_IsRaw(const cJSON *const item);

    /** @brief 创建JSON null；失败 NULL，未移交父节点时由调用方 Delete。
     * 示例：
     * @code
     * cJSON *value = cJSON_CreateNull();
     * cJSON_Delete(value);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_CreateNull(void);
    /** @brief 创建JSON true；失败 NULL，未移交父节点时由调用方 Delete。
     * 示例：
     * @code
     * cJSON *value = cJSON_CreateTrue();
     * cJSON_Delete(value);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_CreateTrue(void);
    /** @brief 创建JSON false；失败 NULL，未移交父节点时由调用方 Delete。
     * 示例：
     * @code
     * cJSON *value = cJSON_CreateFalse();
     * cJSON_Delete(value);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_CreateFalse(void);
    /** @brief 创建布尔值（非零为 true）；失败 NULL，未移交父节点时由调用方 Delete。
     * 示例：
     * @code
     * cJSON *value = cJSON_CreateBool(1);
     * cJSON_Delete(value);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_CreateBool(cJSON_bool boolean);
    /** @brief 创建double 数值（大整数可能丢失精度）；失败 NULL，未移交父节点时由调用方 Delete。
     * 示例：
     * @code
     * cJSON *value = cJSON_CreateNumber(3.5);
     * cJSON_Delete(value);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_CreateNumber(double num);
    /** @brief 创建字符串副本；失败 NULL，未移交父节点时由调用方 Delete。
     * 示例：
     * @code
     * cJSON *value = cJSON_CreateString("person");
     * cJSON_Delete(value);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_CreateString(const char *string);
    /** @brief 创建原始 JSON 文本副本，不验证语法；失败 NULL，未移交父节点时由调用方 Delete。
     * 示例：
     * @code
     * cJSON *value = cJSON_CreateRaw("[1,2]");
     * cJSON_Delete(value);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_CreateRaw(const char *raw);
    /** @brief 创建空数组；失败 NULL，未移交父节点时由调用方 Delete。
     * 示例：
     * @code
     * cJSON *value = cJSON_CreateArray();
     * cJSON_Delete(value);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_CreateArray(void);
    /** @brief 创建空对象；失败 NULL，未移交父节点时由调用方 Delete。
     * 示例：
     * @code
     * cJSON *value = cJSON_CreateObject();
     * cJSON_Delete(value);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_CreateObject(void);

    /** @brief 创建引用字符串的节点，不复制/释放字符；源字符须比节点存活更久。
     * 示例：
     * @code
     * cJSON *value = cJSON_CreateStringReference("person");
     * cJSON_Delete(value);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_CreateStringReference(const char *string);
    /** @brief 创建借用子项链表的对象节点；不复制/释放 child 链表，原拥有者必须更晚销毁。
     * 示例：
     * @code
     * cJSON *ref = cJSON_CreateObjectReference(root->child);
     * cJSON_Delete(ref);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_CreateObjectReference(const cJSON *child);
    /** @brief 创建借用子项链表的数组节点；不复制/释放 child 链表，原拥有者必须更晚销毁。
     * 示例：
     * @code
     * cJSON *ref = cJSON_CreateArrayReference(array->child);
     * cJSON_Delete(ref);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_CreateArrayReference(const cJSON *child);

    /** @brief 复制输入的 count 项到新 JSON 数组；负 count 或无输入返回 NULL，字符串会深拷贝。
     * 示例：
     * @code
     * int values[] = {1, 2};
     * cJSON *a = cJSON_CreateIntArray(values, 2);
     * cJSON_Delete(a);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_CreateIntArray(const int *numbers, int count);
    /** @brief 复制输入的 count 项到新 JSON 数组；负 count 或无输入返回 NULL，字符串会深拷贝。
     * 示例：
     * @code
     * float values[] = {0.1f, 0.2f};
     * cJSON *a = cJSON_CreateFloatArray(values, 2);
     * cJSON_Delete(a);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_CreateFloatArray(const float *numbers, int count);
    /** @brief 复制输入的 count 项到新 JSON 数组；负 count 或无输入返回 NULL，字符串会深拷贝。
     * 示例：
     * @code
     * double values[] = {0.1, 0.2};
     * cJSON *a = cJSON_CreateDoubleArray(values, 2);
     * cJSON_Delete(a);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_CreateDoubleArray(const double *numbers, int count);
    /** @brief 复制输入的 count 项到新 JSON 数组；负 count 或无输入返回 NULL，字符串会深拷贝。
     * 示例：
     * @code
     * const char *values[] = {"a", "b"};
     * cJSON *a = cJSON_CreateStringArray(values, 2);
     * cJSON_Delete(a);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_CreateStringArray(const char **strings, int count);

    /** @brief 把独立 item 追加到数组，成功挂入后所有权归数组；勿重复挂载或单独 Delete。
     * 示例：
     * @code
     * cJSON *a = cJSON_CreateArray();
     * cJSON_AddItemToArray(a, cJSON_CreateNumber(3));
     * cJSON_Delete(a);
     * @endcode
     */
    CJSON_PUBLIC(void) cJSON_AddItemToArray(cJSON *array, cJSON *item);
    /** @brief 将独立 item 挂到对象并复制键；不替换同名项，后续由父树释放。
     * @note 此版本返回 void；需要可检查的创建/添加操作时用 AddNumberToObject 等类型化接口。
     * 示例：
     * @code
     * cJSON *o = cJSON_CreateObject();
     * cJSON_AddItemToObject(o, "count", cJSON_CreateNumber(3));
     * cJSON_Delete(o);
     * @endcode
     */
    CJSON_PUBLIC(void) cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item);
    /** @brief 添加成员并借用常量键名；键须比对象存活更久，item 所有权交给父树。
     * 示例：
     * @code
     * cJSON *o = cJSON_CreateObject();
     * cJSON_AddItemToObjectCS(o, "count", cJSON_CreateNumber(3));
     * cJSON_Delete(o);
     * @endcode
     */
    CJSON_PUBLIC(void) cJSON_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item);
    /** @brief 追加 item 的引用节点，不接管原 item；源值须在引用使用期间存活。
     * 示例：
     * @code
     * cJSON *a = cJSON_CreateArray();
     * cJSON_AddItemReferenceToArray(a, item);
     * cJSON_Delete(a);
     * @endcode
     */
    CJSON_PUBLIC(void) cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item);
    /** @brief 以复制的键添加 item 引用，不接管原 item，源值须更晚销毁。
     * 示例：
     * @code
     * cJSON *o = cJSON_CreateObject();
     * cJSON_AddItemReferenceToObject(o, "copy", item);
     * cJSON_Delete(o);
     * @endcode
     */
    CJSON_PUBLIC(void) cJSON_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item);

    /** @brief 从 parent 摘下确实属于它的 item；返回独立节点，由调用方负责释放。
     * 示例：
     * @code
     * cJSON *child = cJSON_GetArrayItem(array, 0);
     * cJSON *owned = cJSON_DetachItemViaPointer(array, child);
     * cJSON_Delete(owned);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_DetachItemViaPointer(cJSON *parent, cJSON *const item);
    /** @brief 摘下数组第 which 项（从 0 开始）；越界 NULL，成功节点不再由数组释放。
     * 示例：
     * @code
     * cJSON *owned = cJSON_DetachItemFromArray(array, 0);
     * cJSON_Delete(owned);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromArray(cJSON *array, int which);
    /** @brief 摘下并释放数组第 which 项，越界不操作。
     * 示例：
     * @code
     * cJSON_DeleteItemFromArray(array, 0);
     * @endcode
     */
    CJSON_PUBLIC(void) cJSON_DeleteItemFromArray(cJSON *array, int which);
    /** @brief 按不区分大小写的键摘下首个成员；未找到 NULL，返回节点需自行释放。
     * 示例：
     * @code
     * cJSON *owned = cJSON_DetachItemFromObject(root, "count");
     * cJSON_Delete(owned);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromObject(cJSON *object, const char *string);
    /** @brief 按区分大小写的键摘下首个成员；未找到 NULL，返回节点需自行释放。
     * 示例：
     * @code
     * cJSON *owned = cJSON_DetachItemFromObjectCaseSensitive(root, "count");
     * cJSON_Delete(owned);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromObjectCaseSensitive(cJSON *object, const char *string);
    /** @brief 按不区分大小写的键摘下并释放首个成员；未找到不操作。
     * 示例：
     * @code
     * cJSON_DeleteItemFromObject(root, "count");
     * @endcode
     */
    CJSON_PUBLIC(void) cJSON_DeleteItemFromObject(cJSON *object, const char *string);
    /** @brief 按区分大小写的键摘下并释放首个成员；未找到不操作。
     * 示例：
     * @code
     * cJSON_DeleteItemFromObjectCaseSensitive(root, "count");
     * @endcode
     */
    CJSON_PUBLIC(void) cJSON_DeleteItemFromObjectCaseSensitive(cJSON *object, const char *string);

    /** @brief 在非负 which 前插入独立节点，其后项右移；超过尾部时追加，成功后数组拥有它。
     * 示例：
     * @code
     * cJSON *a = cJSON_CreateArray();
     * cJSON_InsertItemInArray(a, 0, cJSON_CreateNumber(3));
     * cJSON_Delete(a);
     * @endcode
     */
    CJSON_PUBLIC(void) cJSON_InsertItemInArray(cJSON *array, int which, cJSON *newitem);
    /** @brief 替换 parent 中的 item 并释放旧节点；成功返回 1 且父节点接管 replacement。
     * @note item 必须属于 parent；失败返回 0，replacement 仍由调用方负责。
     * 示例：
     * @code
     * cJSON *old = cJSON_GetObjectItemCaseSensitive(root, "count");
     * cJSON *next = cJSON_CreateNumber(4);
     * if (!cJSON_ReplaceItemViaPointer(root, old, next))
     *     cJSON_Delete(next);
     * @endcode
     */
    CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemViaPointer(cJSON *const parent, cJSON *const item, cJSON *replacement);
    /** @brief 替换已存在的非负数组下标并释放旧项；此版本无返回值，先确认下标存在。
     * 示例：
     * @code
     * if (cJSON_GetArrayItem(array, 0))
     *     cJSON_ReplaceItemInArray(array, 0, cJSON_CreateNumber(4));
     * @endcode
     */
    CJSON_PUBLIC(void) cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem);
    /** @brief 按不区分大小写的键替换首个成员并释放旧项；先确认成员存在，失败不接管新项。
     * 示例：
     * @code
     * if (cJSON_GetObjectItem(root, "count"))
     *     cJSON_ReplaceItemInObject(root, "count", cJSON_CreateNumber(4));
     * @endcode
     */
    CJSON_PUBLIC(void) cJSON_ReplaceItemInObject(cJSON *object, const char *string, cJSON *newitem);
    /** @brief 按区分大小写的键替换首个成员并释放旧项；先确认成员存在，失败不接管新项。
     * 示例：
     * @code
     * if (cJSON_GetObjectItemCaseSensitive(root, "count"))
     *     cJSON_ReplaceItemInObjectCaseSensitive(root, "count", cJSON_CreateNumber(4));
     * @endcode
     */
    CJSON_PUBLIC(void) cJSON_ReplaceItemInObjectCaseSensitive(cJSON *object, const char *string, cJSON *newitem);

    /** @brief 创建独立副本；recurse 非 0 深拷贝子树，为 0 仅复制当前节点。
     * @return 失败 NULL；副本用 Delete 释放，next/prev 不指向原兄弟节点。
     * 示例：
     * @code
     * cJSON *copy = cJSON_Duplicate(root, 1);
     * cJSON_Delete(copy);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_Duplicate(const cJSON *item, cJSON_bool recurse);
    /* Duplicate will create a new, identical cJSON item to the one you pass, in new memory that will
    need to be released. With recurse!=0, it will duplicate any children connected to the item.
    The item->next and ->prev pointers are always zero on return from Duplicate. */
    /** @brief 递归比较内容，返回 1/0；case_sensitive 控制对象键大小写，NULL/无效节点不相等。
     * 示例：
     * @code
     * int equal = cJSON_Compare(root, other, 1);
     * @endcode
     */
    CJSON_PUBLIC(cJSON_bool) cJSON_Compare(const cJSON *const a, const cJSON *const b, const cJSON_bool case_sensitive);

    /** @brief 原位移除 JSON 文本中非字符串部分的空白/注释；不验证 JSON，需可写零结尾缓冲区。
     * 示例：
     * @code
     * char text[] = "[ 1, 2 ]";
     * cJSON_Minify(text);
     * @endcode
     */
    CJSON_PUBLIC(void) cJSON_Minify(char *json);

    /** @brief 创建JSON null并添加到对象；成功返回父树拥有的借用节点，失败 NULL。
     * 示例：
     * @code
     * cJSON *o = cJSON_CreateObject();
     * cJSON *added = cJSON_AddNullToObject(o, "value");
     * (void)added;
     * cJSON_Delete(o);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_AddNullToObject(cJSON *const object, const char *const name);
    /** @brief 创建JSON true并添加到对象；成功返回父树拥有的借用节点，失败 NULL。
     * 示例：
     * @code
     * cJSON *o = cJSON_CreateObject();
     * cJSON *added = cJSON_AddTrueToObject(o, "value");
     * (void)added;
     * cJSON_Delete(o);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_AddTrueToObject(cJSON *const object, const char *const name);
    /** @brief 创建JSON false并添加到对象；成功返回父树拥有的借用节点，失败 NULL。
     * 示例：
     * @code
     * cJSON *o = cJSON_CreateObject();
     * cJSON *added = cJSON_AddFalseToObject(o, "value");
     * (void)added;
     * cJSON_Delete(o);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_AddFalseToObject(cJSON *const object, const char *const name);
    /** @brief 创建布尔值并添加到对象；成功返回父树拥有的借用节点，失败 NULL。
     * 示例：
     * @code
     * cJSON *o = cJSON_CreateObject();
     * cJSON *added = cJSON_AddBoolToObject(o, "value", 1);
     * (void)added;
     * cJSON_Delete(o);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_AddBoolToObject(cJSON *const object, const char *const name, const cJSON_bool boolean);
    /** @brief 创建double 数值并添加到对象；成功返回父树拥有的借用节点，失败 NULL。
     * 示例：
     * @code
     * cJSON *o = cJSON_CreateObject();
     * cJSON *added = cJSON_AddNumberToObject(o, "value", 3.5);
     * (void)added;
     * cJSON_Delete(o);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_AddNumberToObject(cJSON *const object, const char *const name, const double number);
    /** @brief 创建字符串副本并添加到对象；成功返回父树拥有的借用节点，失败 NULL。
     * 示例：
     * @code
     * cJSON *o = cJSON_CreateObject();
     * cJSON *added = cJSON_AddStringToObject(o, "value", "person");
     * (void)added;
     * cJSON_Delete(o);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *)
    cJSON_AddStringToObject(cJSON *const object, const char *const name, const char *const string);
    /** @brief 创建原始 JSON 文本（调用方保证有效）并添加到对象；成功返回父树拥有的借用节点，失败 NULL。
     * 示例：
     * @code
     * cJSON *o = cJSON_CreateObject();
     * cJSON *added = cJSON_AddRawToObject(o, "value", "[1,2]");
     * (void)added;
     * cJSON_Delete(o);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_AddRawToObject(cJSON *const object, const char *const name, const char *const raw);
    /** @brief 创建空对象并添加到对象；成功返回父树拥有的借用节点，失败 NULL。
     * 示例：
     * @code
     * cJSON *o = cJSON_CreateObject();
     * cJSON *added = cJSON_AddObjectToObject(o, "value");
     * (void)added;
     * cJSON_Delete(o);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_AddObjectToObject(cJSON *const object, const char *const name);
    /** @brief 创建空数组并添加到对象；成功返回父树拥有的借用节点，失败 NULL。
     * 示例：
     * @code
     * cJSON *o = cJSON_CreateObject();
     * cJSON *added = cJSON_AddArrayToObject(o, "value");
     * (void)added;
     * cJSON_Delete(o);
     * @endcode
     */
    CJSON_PUBLIC(cJSON *) cJSON_AddArrayToObject(cJSON *const object, const char *const name);

/** @brief 给数值节点写小整数并同步两种数值字段；参数避免副作用，大数用 SetNumberValue。
 * 示例：
 * @code
 * cJSON *n = cJSON_CreateNumber(0);
 * cJSON_SetIntValue(n, 3);
 * cJSON_Delete(n);
 * @endcode
 */
#define cJSON_SetIntValue(object, number) ((object) ? (object)->valueint = (object)->valuedouble = (number) : (number))
    /** @brief 更新数值节点的 double 和饱和 int 字段并返回 number；一般使用 SetNumberValue 宏。
     * 示例：
     * @code
     * cJSON *n = cJSON_CreateNumber(0);
     * cJSON_SetNumberHelper(n, 3.5);
     * cJSON_Delete(n);
     * @endcode
     */
    CJSON_PUBLIC(double) cJSON_SetNumberHelper(cJSON *object, double number);
/** @brief 更新数值节点并返回 number；valueint 超出范围时饱和，参数应为普通变量。
 * 示例：
 * @code
 * cJSON *n = cJSON_CreateNumber(0);
 * cJSON_SetNumberValue(n, 3.5);
 * cJSON_Delete(n);
 * @endcode
 */
#define cJSON_SetNumberValue(object, number)                                                                           \
    ((object != NULL) ? cJSON_SetNumberHelper(object, (double)number) : (number))

/** @brief 按链表遍历数组/对象的借用子项；遍历中不要释放当前项。
 * 示例：
 * @code
 * cJSON *entry = NULL;
 * cJSON_ArrayForEach(entry, array)
 * {
 *     (void)entry->type;
 * }
 * @endcode
 */
#define cJSON_ArrayForEach(element, array)                                                                             \
    for (element = (array != NULL) ? (array)->child : NULL; element != NULL; element = element->next)

    /** @brief 使用当前 cJSON 分配器分配 size 字节；失败 NULL，配对 cJSON_free。
     * 示例：
     * @code
     * void *memory = cJSON_malloc(128);
     * cJSON_free(memory);
     * @endcode
     */
    CJSON_PUBLIC(void *) cJSON_malloc(size_t size);
    /** @brief 使用当前 cJSON 释放器释放内存；用于 Print/malloc 返回值，树用 Delete。
     * 示例：
     * @code
     * char *text = cJSON_PrintUnformatted(root);
     * cJSON_free(text);
     * @endcode
     */
    CJSON_PUBLIC(void) cJSON_free(void *object);
#ifdef __cplusplus
}
#endif

#endif
