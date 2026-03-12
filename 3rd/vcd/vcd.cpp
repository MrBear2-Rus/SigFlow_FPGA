#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vcd.h"

// 扩展支持的赋值值字符（包含x/z/X/Z）
#define isexpression(c) (strchr("-0123456789zZxXbU", (c)))

#define BUFFER_LENGTH 512

typedef enum {
    BEFORE_MODULE_DEFINITIONS,
    INSIDE_TOP_MODULE,
    INSIDE_INNER_MODULES
} state_t;

static vcd_t* new_vcd();
static bool parse_instruction(FILE* file, vcd_t* vcd, state_t* state);
static bool parse_timestamp(FILE* file, timestamp_t* timestamp);
static bool parse_assignment(FILE* file, vcd_t* vcd, timestamp_t timestamp);
static int get_signal_index(const char* string);
// 新增：日志辅助函数（调试用，可按需删除）
static void log_signal_info(vcd_t* vcd);
// 新增：模块路径管理函数声明（核心新增）
static void update_module_path(vcd_t* vcd, const char* module_name, bool is_upscope);

vcd_t* vcd_read_from_path(char* path) {
    FILE* file = fopen(path, "r");
    if (file == NULL)
        return NULL;

    vcd_t* vcd = new_vcd();
    timestamp_t current_timestamp = 0;
    state_t state = BEFORE_MODULE_DEFINITIONS;

    int character = 0;
    while ((character = fgetc(file)) != EOF) {
        if (character == '$') {
            bool successful = parse_instruction(file, vcd, &state);
            if (successful)
                continue;
        }
        else if (character == '#') {
            bool successful = parse_timestamp(file, &current_timestamp);
            if (successful)
                continue;
        }
        else if (isexpression(character)) {
            ungetc(character, file);
            bool successful = parse_assignment(file, vcd, current_timestamp);
            if (successful)
                continue;
        }
        else if (isspace(character)) {
            continue; // 跳过空白字符
        }
        else {
            // 原逻辑错误：遇到未知字符直接释放资源返回NULL，改为跳过
            continue;
        }
    }

    // 调试：打印解析到的信号信息
    log_signal_info(vcd);

    fclose(file);
    return vcd;
}

signal_t* vcd_get_signal_by_name(vcd_t* vcd, const char* signal_name) {
    if (vcd == NULL || signal_name == NULL) return NULL;
    for (size_t i = 0; i < vcd->signals_count; ++i) { // 修复：int -> size_t 避免溢出
        // 修改：匹配完整信号名（模块路径+信号名），而非仅原始名称
        if (strcmp(vcd->signals[i].full_name, signal_name) == 0)
            return &vcd->signals[i];
    }
    return NULL;
}

char* vcd_signal_get_value_at_timestamp(signal_t* signal, timestamp_t timestamp) {
    if (signal == NULL) return NULL;
    char* previous_value = NULL;
    for (size_t i = 0; i < signal->changes_count; ++i) { // 修复：int -> size_t
        value_change_t* value_change = &signal->value_changes[i];
        if (timestamp < value_change->timestamp)
            break;
        previous_value = value_change->value;
    }
    // 修复：如果没有找到任何值，返回空字符串而非NULL（避免野指针）
    return previous_value ? previous_value : (char*)"";
}

// 新增：内存释放函数（解决内存泄漏）
void vcd_free(vcd_t* vcd) {
    if (vcd) {
        free(vcd);
    }
}

vcd_t* new_vcd() {
    vcd_t* vcd = (vcd_t*)calloc(1, sizeof(vcd_t));
    // 初始化默认值，避免空指针
    if (vcd) {
        memset(vcd->date, 0, VCD_DATE_SIZE);
        memset(vcd->version, 0, VCD_VERSION_SIZE);
        memset(vcd->timescale.unit, 0, VCD_TIME_UNIT_SIZE);
        vcd->timescale.scale = 1;
        // 新增：初始化当前模块路径为空
        memset(vcd->current_module_path, 0, VCD_NAME_SIZE);
    }
    return vcd;
}

// 新增：模块路径拼接/回退核心函数
static void update_module_path(vcd_t* vcd, const char* module_name, bool is_upscope) {
    if (vcd == NULL) return;

    if (is_upscope) {
        // 回退路径：如 TOP.full_adder → TOP
        char* last_dot = strrchr(vcd->current_module_path, '.');
        if (last_dot != NULL) {
            *last_dot = '\0'; // 截断最后一个模块名
        }
        else {
            memset(vcd->current_module_path, 0, VCD_NAME_SIZE); // 清空路径
        }
    }
    else {
        // 拼接路径：如 TOP + full_adder → TOP.full_adder
        if (module_name == NULL || *module_name == '\0') return;

        if (strlen(vcd->current_module_path) > 0) {
            snprintf(vcd->current_module_path, VCD_NAME_SIZE,
                "%s.%s", vcd->current_module_path, module_name);
        }
        else {
            strncpy(vcd->current_module_path, module_name, VCD_NAME_SIZE - 1);
        }
        vcd->current_module_path[VCD_NAME_SIZE - 1] = '\0'; // 防止溢出
    }
}

bool parse_instruction(FILE* file, vcd_t* vcd, state_t* state) {
    char instruction[BUFFER_LENGTH];
    if (fscanf(file, "%s", instruction) != 1)
        return false;

    // 合并重复的指令判断
    if (strcmp(instruction, "end") == 0 || strcmp(instruction, "dumpvars") == 0 ||
        strcmp(instruction, "dumpall") == 0 || strcmp(instruction, "comment") == 0) {
        return true;
    }

    if (strcmp(instruction, "scope") == 0) {
        char module_type[BUFFER_LENGTH], module_name[BUFFER_LENGTH];
        fscanf(file, " %s %s", module_type, module_name); // 读取模块类型（如module）和名称（如TOP）
        update_module_path(vcd, module_name, false); // 新增：调用路径拼接函数，进入模块时拼接路径
        // 原状态切换逻辑保留
        switch (*state) {
        case BEFORE_MODULE_DEFINITIONS:
            *state = INSIDE_TOP_MODULE;
            break;
        case INSIDE_TOP_MODULE:
            *state = INSIDE_INNER_MODULES;
            break;
        default:
            break;
        }
        fscanf(file, "\n%*[^$]");
        return true;
    }

    if (strcmp(instruction, "upscope") == 0 || strcmp(instruction, "enddefinitions") == 0) {
        // 修复：upscope时恢复状态+回退模块路径
        if (strcmp(instruction, "upscope") == 0) {
            update_module_path(vcd, NULL, true); // 新增：退出模块时回退路径
            if (*state == INSIDE_INNER_MODULES) {
                *state = INSIDE_TOP_MODULE; // 恢复状态
            }
        }
        fscanf(file, "\n%*[^$]");
        return true;
    }

    if (strcmp(instruction, "var") == 0) {
        // 修复：允许inner模块的信号解析（按需调整，若需要过滤可保留原逻辑）
        // if (*state == INSIDE_INNER_MODULES) {
        //     fscanf(file, " %*[^\n]\n");
        //     return true;
        // }

        // 保护：避免信号数量超过上限
        if (vcd->signals_count >= VCD_SIGNAL_COUNT) {
            fscanf(file, " %*[^\n]\n");
            return true;
        }

        signal_t* signal = &vcd->signals[vcd->signals_count];
        // 初始化信号结构体，避免脏数据
        memset(signal, 0, sizeof(signal_t));
        signal->size = 0;
        signal->changes_count = 0;

        char signal_id[VCD_NAME_SIZE];
        char type[BUFFER_LENGTH]; // 存储var类型（如wire/reg）
        // 修复：格式化字符串，避免读取越界
        if (fscanf(file, " %s %zu %[^ ] %[^ $]%*[^$]",
            type, &signal->size, signal_id, signal->name) != 4) {
            return false;
        }

        // 新增：存储信号ID（用于赋值解析时精准匹配）
        strncpy(signal->signal_id, signal_id, VCD_NAME_SIZE - 1);
        signal->signal_id[VCD_NAME_SIZE - 1] = '\0';

        // 新增：复制当前模块路径到信号
        strncpy(signal->module_path, vcd->current_module_path, VCD_NAME_SIZE - 1);
        signal->module_path[VCD_NAME_SIZE - 1] = '\0';

        // 新增：生成完整信号名（模块路径+原始名称）
        if (strlen(signal->module_path) > 0) {
            snprintf(signal->full_name, VCD_NAME_SIZE,
                "%s.%s", signal->module_path, signal->name);
        }
        else {
            strncpy(signal->full_name, signal->name, VCD_NAME_SIZE - 1);
        }
        signal->full_name[VCD_NAME_SIZE - 1] = '\0';

        int index = get_signal_index(signal_id);
        // 修复：别名信号处理（复用原信号的配置）
        if (index >= 0 && index < VCD_SIGNAL_COUNT && vcd->signals[index].size != 0) {
            memcpy(signal, &vcd->signals[index], sizeof(signal_t));
        }

        vcd->signals_count += 1;
        return true;
    }

    if (strcmp(instruction, "date") == 0) {
        fscanf(file, "\n%[^$\n]", vcd->date);
        return true;
    }

    if (strcmp(instruction, "version") == 0) {
        fscanf(file, "\n%[^$\n]", vcd->version);
        return true;
    }

    if (strcmp(instruction, "timescale") == 0) {
        // 修复：timescale解析格式（适配常见格式如 1ns/10ps）
        fscanf(file, "\n%zu%[^ \n$]", &vcd->timescale.scale, vcd->timescale.unit);
        return true;
    }

    return false;
}

bool parse_timestamp(FILE* file, timestamp_t* timestamp) {
    // 修复：跳过时间戳前的空白字符，提高兼容性
    return fscanf(file, " %u", timestamp) == 1;
}

bool parse_assignment(FILE* file, vcd_t* vcd, timestamp_t timestamp) {
    char buffer[BUFFER_LENGTH];
    if (fgets(buffer, BUFFER_LENGTH, file) == NULL) return false;

    char value[VCD_SIGNAL_SIZE] = { 0 };
    char signal_id[VCD_NAME_SIZE] = { 0 };
    bool is_vector = (strchr("01xXzZ", buffer[0]) == NULL);

    // 修复：赋值格式解析（区分标量/向量）
    int parse_count = 0;
    if (is_vector) {
        // 向量格式：b<值> <ID> （如 b101 !）
        parse_count = sscanf(buffer, "%[^ ] %[^\n ]", value, signal_id);
    }
    else {
        // 标量格式：<值><ID> （如 1!）
        parse_count = sscanf(buffer, "%1s%[^\n ]", value, signal_id);
    }

    if (parse_count != 2) return false;

    // 修复：取消长ID忽略（若需限制可改为截断，而非直接跳过）
    if (strlen(signal_id) >= VCD_NAME_SIZE) {
        signal_id[VCD_NAME_SIZE - 1] = '\0'; // 截断超长ID
    }

    // 修改：按signal_id精准匹配信号（替代原get_signal_index）
    int target_index = -1;
    for (size_t i = 0; i < vcd->signals_count; ++i) {
        if (strcmp(vcd->signals[i].signal_id, signal_id) == 0) {
            target_index = (int)i;
            break;
        }
    }

    // 修复：索引合法性校验（包含信号数量上限）
    if (target_index < 0 || target_index >= VCD_SIGNAL_COUNT || target_index >= vcd->signals_count) {
        return true;
    }

    signal_t* signal = &vcd->signals[target_index];
    // 保护：避免值变化数量超过上限
    if (signal->changes_count >= VCD_VALUE_CHANGE_COUNT) {
        return true;
    }

    // 赋值：存储时间戳和值
    value_change_t* change = &signal->value_changes[signal->changes_count];
    change->timestamp = timestamp;
    strncpy(change->value, value, VCD_SIGNAL_SIZE - 1); // 留末尾'\0'
    change->value[VCD_SIGNAL_SIZE - 1] = '\0'; // 确保字符串结束
    signal->changes_count += 1;

    // 调试：打印赋值信息（新增完整信号名）
    printf("赋值 -> ID:%s 完整名称:%s 时间戳:%u 值:%s\n",
        signal_id, signal->full_name, timestamp, value);
    return true;
}

// 保留原函数（兼容历史逻辑，实际赋值解析已改用signal_id匹配）
int get_signal_index(const char* string) {
    if (string == NULL || *string == '\0') return -1;

    int id = -1;
    // 适配数字ID（0-9）
    if (isdigit((unsigned char)*string)) {
        id = *string - '0';
    }
    // 适配小写字母ID（a-z）
    else if (islower((unsigned char)*string)) {
        id = *string - 'a' + 10;
    }
    // 适配大写字母ID（A-Z）
    else if (isupper((unsigned char)*string)) {
        id = *string - 'A' + 36;
    }
    // 保留原逻辑（!开头）
    else {
        id = *string - '!';
    }

    // 校验：ID不能超过信号数量上限
    if (id >= 0 && id < VCD_SIGNAL_COUNT) {
        return id;
    }
    return -1;
}

// 新增：调试日志 - 打印解析到的所有信号信息（含模块路径）
static void log_signal_info(vcd_t* vcd) {
    if (vcd == NULL) return;
    printf("===== VCD解析结果 =====\n");
    printf("信号总数：%zu\n", vcd->signals_count);
    for (size_t i = 0; i < vcd->signals_count; ++i) {
        signal_t* sig = &vcd->signals[i];
        printf("信号[%zu]：原始名称=%s | 模块路径=%s | 完整名称=%s | ID=%s | 位宽=%zu | 变化次数=%zu\n",
            i, sig->name, sig->module_path, sig->full_name, sig->signal_id,
            sig->size, sig->changes_count);
    }
    printf("========================\n");
}
timestamp_t vcd_get_max_timestamp(vcd_t* vcd) {
    // 入参合法性校验
    if (vcd == NULL) {
        return 0;
    }

    timestamp_t max_ts = 0;

    // 遍历所有信号
    for (size_t i = 0; i < vcd->signals_count; ++i) {
        signal_t* signal = &vcd->signals[i];

        // 遍历当前信号的所有值变化记录
        for (size_t j = 0; j < signal->changes_count; ++j) {
            value_change_t* change = &signal->value_changes[j];

            // 更新最大时间戳
            if (change->timestamp > max_ts) {
                max_ts = change->timestamp;
            }
        }
    }

    return max_ts;
}