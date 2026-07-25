/* =========================================================================
 * wake.c —— 离线唤醒词（ESP-SR WakeNet）的具体实现（L8 / L9 重写）
 * -------------------------------------------------------------------------
 * 用 #if __has_include("esp_wn_iface.h") 在【编译期】判断本机有没有装 esp-sr：
 *   装了  → USE_WAKENET=1，走真实 WakeNet 推理（下方 #if 分支）
 *   没装  → USE_WAKENET=0，走模拟模式（wake_feed 永远返回 false）
 *
 * ⚠️ 重要：真实 esp-sr 的 API 不是 wakenet.h / wakenet_init，那套是早期占位写法，
 *   现代 esp-sr（v1.x~v2.x）的正确用法是：
 *     1) esp_srmodel_init("model")           从 flash 的 model 分区读出所有模型列表
 *     2) esp_srmodel_filter(list, "wn", NULL) 过滤出第一个 WakeNet 模型名
 *     3) esp_wn_handle_from_name(name)        拿到该模型的算子接口 esp_wn_iface_t*
 *     4) iface->create(name, DET_MODE_90)     创建模型实例 model_iface_data_t*
 *     5) iface->get_samp_chunksize(data)      查询每次 detect 需要喂多少样本
 *     6) iface->detect(data, chunk)           喂一个 chunk，返回是否命中
 *   头文件：esp_wn_iface.h / esp_wn_models.h（arch 目录下）、model_path.h（src/include）。
 *
 * 喂数据约定：mic 每帧喂 512 样本（32ms@16k），但 WakeNet 要求每次 detect 正好喂
 *   get_samp_chunksize() 个样本（wn9 通常也是 512，但不保证）。所以这里用一个内部
 *   累积缓冲，凑够一个 chunk 再送进去检测，多余的留到下次——无论 chunk 多大都稳。
 * ========================================================================= */

#include "wake.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "wake";

/* 探测：只要能找到 esp_wn_iface.h，就认为 esp-sr 可用 */
#if __has_include("esp_wn_iface.h")
#define USE_WAKENET 1
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

/* model 分区标签（和 partitions.csv 里的 name 一致） */
#define WAKE_MODEL_PARTITION_LABEL  "model"

static srmodel_list_t     *s_models   = NULL;  // flash 里的模型列表
static const esp_wn_iface_t *s_iface  = NULL;  // WakeNet 算子接口
static model_iface_data_t  *s_data    = NULL;  // WakeNet 模型实例

static int      s_chunk    = 0;     // 每次 detect 需要的样本数
static int16_t *s_acc      = NULL;  // 累积缓冲（长度 = s_chunk）
static int      s_acc_len  = 0;     // 累积缓冲当前已填的样本数
static int64_t  s_cooldown_until = 0; // 触发后的冷却截止时间(us)，防止连触发
#define WAKE_COOLDOWN_US  (1500000) // 1.5s 内不重复触发

#else
#define USE_WAKENET 0
#endif

/* -------------------------------------------------------------------------
 * wake_init：从 flash 读模型 → 过滤出 WakeNet → 创建实例 → 备好累积缓冲
 * ------------------------------------------------------------------------- */
bool wake_init(void)
{
#if USE_WAKENET
    /* 1) 读 model 分区里的全部模型 */
    s_models = esp_srmodel_init(WAKE_MODEL_PARTITION_LABEL);
    if (s_models == NULL || s_models->num <= 0) {
        ESP_LOGE(TAG, "model 分区没有任何模型！请确认：");
        ESP_LOGE(TAG, "  1) menuconfig / sdkconfig 选了 WakeNet 模型（如 CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y）");
        ESP_LOGE(TAG, "  2) partitions.csv 有 model 分区且已 idf.py flash（模型随固件一起烧录）");
        return false;
    }

    /* 2) 过滤出第一个 WakeNet 模型（不写死名字，选了哪个词就用哪个） */
    char *model_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL);
    if (model_name == NULL) {
        ESP_LOGE(TAG, "模型列表里没有 WakeNet 模型（只有 MultiNet？请在配置里选一个唤醒词模型）");
        return false;
    }
    ESP_LOGI(TAG, "选用唤醒词模型: %s", model_name);

    /* 3) 拿到该模型的算子接口 */
    s_iface = esp_wn_handle_from_name(model_name);
    if (s_iface == NULL) {
        ESP_LOGE(TAG, "esp_wn_handle_from_name(%s) 失败", model_name);
        return false;
    }

    /* 4) 创建模型实例（DET_MODE_90 = 普通灵敏度，误触发低；要更灵敏可改 DET_MODE_95） */
    s_data = s_iface->create(model_name, DET_MODE_90);
    if (s_data == NULL) {
        ESP_LOGE(TAG, "WakeNet create 失败（可能是 PSRAM 不足）");
        return false;
    }

    /* 5) 查询喂数据约束，并备好累积缓冲 */
    s_chunk = s_iface->get_samp_chunksize(s_data);
    int rate = s_iface->get_samp_rate(s_data);
    if (s_chunk <= 0) {
        ESP_LOGE(TAG, "get_samp_chunksize 返回异常: %d", s_chunk);
        return false;
    }
    s_acc = (int16_t *)malloc((size_t)s_chunk * sizeof(int16_t));
    if (s_acc == NULL) {
        ESP_LOGE(TAG, "累积缓冲分配失败(%d 样本)", s_chunk);
        return false;
    }
    s_acc_len = 0;

    ESP_LOGI(TAG, "✅ WakeNet 离线唤醒词已启用：chunk=%d 样本, 采样率=%dHz（说\"你好小智\"开始对话）",
             s_chunk, rate);
    return true;
#else
    ESP_LOGW(TAG, "未安装 esp-sr 组件 → 离线唤醒词不可用，仅按键唤醒（模拟模式）。");
    return false;
#endif
}

/* -------------------------------------------------------------------------
 * wake_feed：喂一段 PCM，凑够一个 chunk 就检测一次
 * ------------------------------------------------------------------------- */
bool wake_feed(const int16_t *buf, int samples)
{
#if USE_WAKENET
    if (s_data == NULL || s_iface == NULL || s_acc == NULL || buf == NULL || samples <= 0) {
        return false;
    }

    bool detected = false;
    int  off = 0;
    while (off < samples) {
        /* 把外来样本尽量填进累积缓冲 */
        int need = s_chunk - s_acc_len;              // 还差多少凑满一个 chunk
        int avail = samples - off;                   // 外来还剩多少
        int take = (avail < need) ? avail : need;
        memcpy(s_acc + s_acc_len, buf + off, (size_t)take * sizeof(int16_t));
        s_acc_len += take;
        off       += take;

        /* 凑满一个 chunk → 检测一次，然后清空缓冲 */
        if (s_acc_len == s_chunk) {
            wakenet_state_t st = s_iface->detect(s_data, s_acc);
            s_acc_len = 0;
            if (st == WAKENET_DETECTED) {
                int64_t now = esp_timer_get_time();
                if (now >= s_cooldown_until) {
                    s_cooldown_until = now + WAKE_COOLDOWN_US;
                    detected = true;
                    ESP_LOGI(TAG, "WAKENET: 检测到唤醒词（%.1fs 冷却）",
                             (double)WAKE_COOLDOWN_US / 1e6);
                    break;  /* 已唤醒，本帧剩余样本丢弃 */
                }
                /* 冷却期内：忽略本次命中，但模型已消费本 chunk，继续喂 */
            }
        }
    }
    return detected;
#else
    (void)buf;
    (void)samples;
    return false;   // 模拟模式：永不触发，保持仅按键唤醒
#endif
}

/* -------------------------------------------------------------------------
 * wake_deinit：释放资源（一般整个生命周期不调用）
 * ------------------------------------------------------------------------- */
void wake_deinit(void)
{
#if USE_WAKENET
    if (s_iface != NULL && s_data != NULL && s_iface->destroy) {
        s_iface->destroy(s_data);
    }
    s_data = NULL;
    s_iface = NULL;
    if (s_acc != NULL) {
        free(s_acc);
        s_acc = NULL;
    }
    s_acc_len = 0;
    if (s_models != NULL) {
        esp_srmodel_deinit(s_models);
        s_models = NULL;
    }
#endif
}
