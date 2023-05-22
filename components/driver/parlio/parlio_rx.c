/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/cdefs.h>
#include <sys/param.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#if CONFIG_PARLIO_ENABLE_DEBUG_LOG
// The local log level must be defined before including esp_log.h
// Set the maximum log level for this source file
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#endif
#include "esp_log.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_rom_gpio.h"
#include "esp_pm.h"
#include "soc/parlio_periph.h"
#include "hal/parlio_ll.h"
#include "hal/gpio_hal.h"
#include "hal/dma_types.h"
#include "driver/gpio.h"
#include "driver/parlio_rx.h"
#include "parlio_private.h"
#include "esp_memory_utils.h"
#include "esp_clk_tree.h"
#include "esp_attr.h"
#include "esp_private/gdma.h"

static const char *TAG = "parlio-rx";

/**
 * @brief Parlio RX transaction
 */
typedef struct {
    parlio_rx_delimiter_handle_t    delimiter;              /*!< Delimiter of this transaction */
    void                            *payload;               /*!< The payload of this transaction, will be mounted to DMA descriptor */
    size_t                          size;                   /*!< The payload size in byte */
    size_t                          recv_bytes;             /*!< The received bytes of this transaction
                                                                 will be reset when all data filled in the infinite transaction */
    struct {
        uint32_t                    infinite : 1;           /*!< Whether this is an infinite transaction */
    } flags;
} parlio_rx_transaction_t;

/**
 * @brief Parlio RX unit resource management
 */
typedef struct parlio_rx_unit_t {
    /* Unit general Resources */
    int                             unit_id;                /*!< unit id */
    parlio_dir_t                    dir;                    /*!< unit direction */
    parlio_group_t                  *group;                 /*!< group handle */
    parlio_clock_source_t           clk_src;                /*!< clock source of the unit */
    parlio_rx_unit_config_t         cfg;                    /*!< basic configuration of the rx unit */
    bool                            is_enabled;             /*!< State flag that indicates whether the unit is enabled */
    /* Mutex Lock */
    SemaphoreHandle_t               mutex;                  /*!< Mutex lock for concurrence safety,
                                                             *   which should be acquired and released in a same function */

    /* Power Management */
    esp_pm_lock_handle_t            pm_lock;                /*!< power management lock */
#if CONFIG_PM_ENABLE
    char                            pm_lock_name[PARLIO_PM_LOCK_NAME_LEN_MAX]; /*!< pm lock name */
#endif

    /* Transaction Resources */
    QueueHandle_t                   trans_que;              /*!< Static transaction queue handle */
    parlio_rx_transaction_t         curr_trans;             /*!< The current transaction */
    SemaphoreHandle_t               trans_sem;              /*!< Binary semaphore to deliver transaction done signal,
                                                             *   which can be acquired and released between different functions */

    /* DMA Resources */
    gdma_channel_handle_t           dma_chan;               /*!< DMA channel */
    size_t                          max_recv_size;          /*!< Maximum receive size for a normal transaction */
    size_t                          desc_num;               /*!< DMA descriptor number */
    dma_descriptor_t                *dma_descs;             /*!< DMA descriptor array pointer */
    dma_descriptor_t                *curr_desc;             /*!< The pointer of the current descriptor */
    void                            *usr_recv_buf;          /*!< The pointe to the user's receiving buffer */
    /* Infinite transaction specific */
    void                            *dma_buf;               /*!< Additional internal DMA buffer only for infinite transactions */

    /* Callback */
    parlio_rx_event_callbacks_t     cbs;                    /*!< The group of callback function pointers */
    void                            *user_data;             /*!< User data that supposed to be transported to the callback functions */

} parlio_rx_unit_t;

/**
 * @brief Delimiter mode
 */
typedef enum {
    PARLIO_RX_LEVEL_MODE,                                   /*!< Delimit by the level of valid signal */
    PARLIO_RX_PULSE_MODE,                                   /*!< Delimit by the pulse of valid signal */
    PARLIO_RX_SOFT_MODE,                                    /*!< Delimit by the length of received data */
} parlio_rx_delimiter_mode_t;

/**
 * @brief Pralio RX delimiter management
 */
typedef struct parlio_rx_delimiter_t {
    parlio_rx_delimiter_mode_t      mode;                   /*!< Delimiter mode */
    bool                            under_using;            /*!< Whether this delimiter is under using */

    uint32_t                        valid_sig;
    gpio_num_t                      valid_sig_line_id;      /*!< The data line id for the valid signal */
    parlio_sample_edge_t            sample_edge;            /*!< The sampling edge of the data */
    parlio_bit_pack_order_t         bit_pack_order;         /*!< The order to pack the bit on the data line */
    uint32_t                        eof_data_len;           /*!< The length of the data to trigger the eof interrupt */
    uint32_t                        timeout_ticks;          /*!< The ticks of source clock that can trigger hardware timeout */
    struct {
        uint32_t                    active_level: 1;           /*!< Which level indicates the validation of the transmitting data */
        uint32_t                    start_bit_included: 1;     /*!< Whether data bit is included in the start pulse */
        uint32_t                    end_bit_included: 1;       /*!< Whether data bit is included in the end pulse, only valid when `has_end_pulse` is true */
        uint32_t                    has_end_pulse: 1;          /*!< Whether there's an end pulse to terminate the transaction,
                                                                    if no, the transaction will be terminated by user configured transcation length */
        uint32_t                    pulse_invert: 1;           /*!< Whether to invert the pulse */
    } flags;
} parlio_rx_delimiter_t;


static IRAM_ATTR size_t s_parlio_mount_transaction_buffer(parlio_rx_unit_handle_t rx_unit, parlio_rx_transaction_t *trans)
{
    dma_descriptor_t *p_desc = rx_unit->dma_descs;
    /* Update the current transaction to the next one, and declare the delimiter is under using of the rx unit */
    memcpy(&rx_unit->curr_trans, trans, sizeof(parlio_rx_transaction_t));
    trans->delimiter->under_using = true;

    uint32_t desc_num = trans->size / DMA_DESCRIPTOR_BUFFER_MAX_SIZE_4B_ALIGNED;
    uint32_t remain_num = trans->size % DMA_DESCRIPTOR_BUFFER_MAX_SIZE_4B_ALIGNED;
    /* If there are still data remained, need one more descriptor */
    desc_num += remain_num ? 1 : 0;
    if (trans->flags.infinite && desc_num < 2) {
        /* At least 2 descriptors needed */
        desc_num = 2;
    }
    size_t mount_size = 0;
    size_t offset = 0;
    for (int i = 0; i < desc_num; i++) {
        size_t rest_size = trans->size - offset;
        if (rest_size >= 2 * DMA_DESCRIPTOR_BUFFER_MAX_SIZE_4B_ALIGNED) {
            mount_size = trans->size / desc_num;
        }
        else if (rest_size <= DMA_DESCRIPTOR_BUFFER_MAX_SIZE_4B_ALIGNED) {
            mount_size = (desc_num == 2) && (i == 0) ? rest_size / 2 : rest_size;
        }
        else {
            mount_size = rest_size / 2;
        }
        p_desc[i].buffer = (void *)((uint8_t *)trans->payload + offset);
        p_desc[i].dw0.size = mount_size;
        p_desc[i].dw0.length = mount_size;
        p_desc[i].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
        // Link the descriptor
        if (i > 0) {
            p_desc[i - 1].next = &p_desc[i];
        }
        offset += mount_size;
    }
    /* For infinite transaction, link the descriptor as a ring */
    p_desc[desc_num - 1].next = trans->flags.infinite ? &p_desc[0] : NULL;
    /* Reset the current DMA node */
    rx_unit->curr_desc = p_desc;

    return offset;
}

static IRAM_ATTR void s_parlio_set_delimiter_config(parlio_rx_unit_handle_t rx_unit, parlio_rx_delimiter_handle_t deli)
{
    parlio_hal_context_t *hal = &(rx_unit->group->hal);

    /* Set the clock sampling edge and the bit order */
    parlio_ll_rx_set_sample_clock_edge(hal->regs, deli->sample_edge);
    parlio_ll_rx_set_bit_pack_order(hal->regs, deli->bit_pack_order);

    /* Set receive mode according to the delimiter */
    switch (deli->mode) {
        case PARLIO_RX_LEVEL_MODE:
            /* Select the level receive mode */
            parlio_ll_rx_set_level_recv_mode(hal->regs, deli->flags.active_level);
            parlio_ll_rx_treat_data_line_as_en(hal->regs, deli->valid_sig_line_id);
            break;
        case PARLIO_RX_PULSE_MODE:
            /* Select the pulse receive mode */
            parlio_ll_rx_set_pulse_recv_mode(hal->regs, deli->flags.start_bit_included,
                                            deli->flags.end_bit_included,
                                            !deli->flags.has_end_pulse,
                                            deli->flags.pulse_invert);
            parlio_ll_rx_treat_data_line_as_en(hal->regs, deli->valid_sig_line_id);
            break;
        default:
            /* Select the soft receive mode */
            parlio_ll_rx_set_soft_recv_mode(hal->regs);
            break;
    }

    /* Set EOF configuration */
    if (deli->eof_data_len) {
        /* If EOF data length specified, set the eof condition to data length and set data bytes */
        parlio_ll_rx_set_recv_bit_len(hal->regs, deli->eof_data_len * 8);
        parlio_ll_rx_set_eof_condition(hal->regs, PARLIO_LL_RX_EOF_COND_RX_FULL);
    } else {
        /* If EOF data length not specified, set the eof condition to the external enable signal */
        parlio_ll_rx_set_eof_condition(hal->regs, PARLIO_LL_RX_EOF_COND_EN_INACTIVE);
    }

    /* Set timeout configuration */
    if (deli->timeout_ticks) {
        parlio_ll_rx_enable_timeout(hal->regs, true);
        parlio_ll_rx_set_timeout_thres(hal->regs, deli->timeout_ticks);
    } else {
        parlio_ll_rx_enable_timeout(hal->regs, false);
    }

    /* Set the validation signal if the validation signal number is set for level or pulse delimiter */
    if (deli->mode != PARLIO_RX_SOFT_MODE) {
        esp_rom_gpio_connect_in_signal(rx_unit->cfg.valid_gpio_num, deli->valid_sig, false);
        /* Update the valid_sig_line_num */
        parlio_ll_rx_treat_data_line_as_en(hal->regs, deli->valid_sig_line_id);
    }

    /* Update/synchronize the new configurations */
    parlio_ll_rx_update_config(hal->regs);
}

static esp_err_t s_parlio_rx_unit_set_gpio(parlio_rx_unit_handle_t rx_unit, const parlio_rx_unit_config_t *config)
{
    int group_id = rx_unit->group->group_id;
    int unit_id = rx_unit->unit_id;

    gpio_config_t gpio_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .pull_down_en = false,
        .pull_up_en = true,
    };

    if (config->clk_src == PARLIO_CLK_SRC_EXTERNAL) {
        ESP_RETURN_ON_FALSE(config->clk_gpio_num >= 0, ESP_ERR_INVALID_ARG, TAG, "clk_gpio_num must be set while the clock input from external");
        /* Connect the clock in signal to the GPIO matrix if it is set */
        if (!config->flags.io_no_init) {
            gpio_conf.mode = config->flags.io_loop_back ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT;
            gpio_conf.pin_bit_mask = BIT64(config->clk_gpio_num);
            ESP_RETURN_ON_ERROR(gpio_config(&gpio_conf), TAG, "config clk in GPIO failed");
            gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[config->clk_gpio_num], PIN_FUNC_GPIO);
        }
        esp_rom_gpio_connect_in_signal(config->clk_gpio_num,
                                       parlio_periph_signals.groups[group_id].rx_units[unit_id].clk_in_sig, false);
    }
    else if (config->clk_gpio_num >= 0) {
#if SOC_PARLIO_RX_CLK_SUPPORT_OUTPUT
        gpio_conf.mode = GPIO_MODE_OUTPUT;
        gpio_conf.pin_bit_mask = BIT64(config->clk_gpio_num);
        ESP_RETURN_ON_ERROR(gpio_config(&gpio_conf), TAG, "config clk in GPIO failed");
        gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[config->clk_gpio_num], PIN_FUNC_GPIO);
        esp_rom_gpio_connect_out_signal(config->clk_gpio_num,
                                       parlio_periph_signals.groups[group_id].rx_units[unit_id].clk_out_sig, false, false);
#else
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, TAG, "this target not support to output the clock");
#endif // SOC_PARLIO_RX_CLK_SUPPORT_OUTPUT
    }

    gpio_conf.mode = GPIO_MODE_INPUT;
    if (config->valid_gpio_num >= 0) {
        if (!config->flags.io_no_init) {
            gpio_conf.pin_bit_mask = BIT64(config->valid_gpio_num);
            gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[config->valid_gpio_num], PIN_FUNC_GPIO);
        }
        ESP_RETURN_ON_ERROR(gpio_config(&gpio_conf), TAG, "config data GPIO failed");
        /* Not connect the signal here, the signal is lazy connected until the delimiter takes effect */
    }

    for (int i = 0; i < config->data_width; i++) {
        /* Loop the data_gpio_nums to connect data and valid signals via GPIO matrix */
        if (config->data_gpio_nums[i] >= 0) {
            if (!config->flags.io_no_init) {
                gpio_conf.pin_bit_mask = BIT64(config->data_gpio_nums[i]);
                ESP_RETURN_ON_ERROR(gpio_config(&gpio_conf), TAG, "config data GPIO failed");
                gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[config->data_gpio_nums[i]], PIN_FUNC_GPIO);
            }
            esp_rom_gpio_connect_in_signal(config->data_gpio_nums[i],
                                            parlio_periph_signals.groups[group_id].rx_units[unit_id].data_sigs[i], false);
        } else {
            ESP_LOGW(TAG, "data line %d not assigned", i);
        }
    }

    return ESP_OK;
}

static IRAM_ATTR bool s_parlio_rx_default_trans_done_callback(gdma_channel_handle_t dma_chan, gdma_event_data_t *event_data, void *user_data)
{
    parlio_rx_unit_handle_t rx_unit = (parlio_rx_unit_handle_t )user_data;
    BaseType_t high_task_woken = pdFALSE;
    bool need_yield = false;

    /* If configured on_receive_done callback and transaction just done */
    if (rx_unit->cbs.on_receive_done) {
        parlio_rx_event_data_t evt_data = {
            .delimiter = rx_unit->curr_trans.delimiter,
            .data = rx_unit->usr_recv_buf,
            .size = rx_unit->curr_trans.size,
            .recv_bytes = rx_unit->curr_trans.recv_bytes,
        };
        need_yield |= rx_unit->cbs.on_receive_done(rx_unit, &evt_data, rx_unit->user_data);
    }

    if (rx_unit->curr_trans.flags.infinite) {
        /* For infinite transactions, reset the receiving bytes when the transaction is done */
        rx_unit->curr_trans.recv_bytes = 0;
    } else {
        parlio_rx_transaction_t next_trans;
        /* The current transaction finished, try to get the next transaction from the transaction queue */
        if (xQueueReceiveFromISR(rx_unit->trans_que, &next_trans, &high_task_woken) == pdTRUE) {
            if (rx_unit->cfg.flags.free_clk) {
                parlio_ll_rx_enable_clock(rx_unit->group->hal.regs, false);
            }
            /* If the delimiter of the next transaction is not same as the current one, need to re-config the hardware */
            if (next_trans.delimiter != rx_unit->curr_trans.delimiter) {
                s_parlio_set_delimiter_config(rx_unit, next_trans.delimiter);
            }
            /* Mount the new transaction buffer and start the new transaction */
            s_parlio_mount_transaction_buffer(rx_unit, &rx_unit->curr_trans);
            gdma_start(rx_unit->dma_chan, (intptr_t)rx_unit->dma_descs);
            if (rx_unit->cfg.flags.free_clk) {
                parlio_ll_rx_start(rx_unit->group->hal.regs, true);
                parlio_ll_rx_enable_clock(rx_unit->group->hal.regs, true);
            }

        } else {
            /* No more transaction pending to receive, clear the current transaction */
            rx_unit->curr_trans.delimiter->under_using = false;
            memset(&rx_unit->curr_trans, 0, sizeof(parlio_rx_transaction_t));
            need_yield |= high_task_woken == pdTRUE;
            xSemaphoreGiveFromISR(rx_unit->trans_sem, &high_task_woken);
        }
    }

    need_yield |= high_task_woken == pdTRUE;
    return need_yield;
}

static IRAM_ATTR bool s_parlio_rx_default_desc_done_callback(gdma_channel_handle_t dma_chan, gdma_event_data_t *event_data, void *user_data)
{
    parlio_rx_unit_handle_t rx_unit = (parlio_rx_unit_handle_t )user_data;
    bool need_yield = false;
    /* Get the finished descriptor from the current descriptor */
    dma_descriptor_t *finished_desc = rx_unit->curr_desc;
    parlio_rx_event_data_t evt_data = {
        .delimiter = rx_unit->curr_trans.delimiter,
        .data = finished_desc->buffer,
        .size = finished_desc->dw0.size,
        .recv_bytes = finished_desc->dw0.length,
    };
    if (rx_unit->cbs.on_partial_receive) {
        need_yield |= rx_unit->cbs.on_partial_receive(rx_unit, &evt_data, rx_unit->user_data);
    }
    /* For the infinite transaction, need to copy the data in DMA buffer to the user receiving buffer */
    if (rx_unit->curr_trans.flags.infinite) {
        memcpy(rx_unit->usr_recv_buf + rx_unit->curr_trans.recv_bytes, evt_data.data, evt_data.recv_bytes);
    } else {
        rx_unit->curr_trans.delimiter->under_using = false;
    }
    /* Update received bytes */
    if (rx_unit->curr_trans.recv_bytes >= rx_unit->curr_trans.size) {
        rx_unit->curr_trans.recv_bytes = 0;
    }
    rx_unit->curr_trans.recv_bytes += evt_data.recv_bytes;
    /* Move to the next DMA descriptor */
    rx_unit->curr_desc = rx_unit->curr_desc->next;

    return need_yield;
}

static IRAM_ATTR bool s_parlio_rx_default_timeout_callback(gdma_channel_handle_t dma_chan, gdma_event_data_t *event_data, void *user_data)
{
    parlio_rx_unit_handle_t rx_unit = (parlio_rx_unit_handle_t )user_data;
    bool need_yield = false;
    parlio_rx_delimiter_handle_t deli = rx_unit->curr_trans.delimiter;
    if (rx_unit->cbs.on_timeout && deli) {
        parlio_rx_event_data_t evt_data = {
            .delimiter = deli,
        };
        need_yield |= rx_unit->cbs.on_timeout(rx_unit, &evt_data, rx_unit->user_data);
    }
    return need_yield;
}

static esp_err_t s_parlio_rx_create_dma_descriptors(parlio_rx_unit_handle_t rx_unit, uint32_t max_recv_size)
{
    ESP_RETURN_ON_FALSE(rx_unit, ESP_ERR_INVALID_ARG, TAG, "invalid param");

    uint32_t desc_num =max_recv_size / DMA_DESCRIPTOR_BUFFER_MAX_SIZE_4B_ALIGNED + 1;
    /* set at least 2 descriptors */
    if (desc_num < 2) {
        desc_num = 4;
    }
    rx_unit->desc_num = desc_num;

    /* Allocated and link the descriptor nodes */
    rx_unit->dma_descs = (dma_descriptor_t *)heap_caps_calloc(desc_num, sizeof(dma_descriptor_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(rx_unit->dma_descs, ESP_ERR_NO_MEM, TAG, "no memory for DMA descriptors");

    rx_unit->max_recv_size = max_recv_size;

    return ESP_OK;
}

static esp_err_t s_parlio_rx_unit_init_dma(parlio_rx_unit_handle_t rx_unit)
{
    /* Allocate and connect the GDMA channel */
    gdma_channel_alloc_config_t dma_chan_config = {
        .direction = GDMA_CHANNEL_DIRECTION_RX,
    };
    ESP_RETURN_ON_ERROR(gdma_new_channel(&dma_chan_config, &rx_unit->dma_chan), TAG, "allocate RX DMA channel failed");
    gdma_connect(rx_unit->dma_chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_PARLIO, 0));

    /* Set GDMA strategy */
    gdma_strategy_config_t gdma_strategy_conf = {
        .auto_update_desc = true,
        .owner_check = false,   // no need to check owner
    };
    gdma_apply_strategy(rx_unit->dma_chan, &gdma_strategy_conf);

    /* Register callbacks */
    gdma_rx_event_callbacks_t cbs = {
        .on_recv_eof = s_parlio_rx_default_trans_done_callback,
        .on_recv_done = s_parlio_rx_default_desc_done_callback,
        // TODO: not on_err_desc, wait for GDMA supports on_err_eof
        // .on_err_eof = s_parlio_rx_default_timeout_callback,
        .on_descr_err = s_parlio_rx_default_timeout_callback,
    };
    gdma_register_rx_event_callbacks(rx_unit->dma_chan, &cbs, rx_unit);

    return ESP_OK;
}

static esp_err_t s_parlio_select_periph_clock(parlio_rx_unit_handle_t rx_unit, const parlio_rx_unit_config_t *config)
{
    parlio_hal_context_t *hal = &rx_unit->group->hal;
    parlio_clock_source_t clk_src = config->clk_src;
    uint32_t periph_src_clk_hz = 0;
    uint32_t div = 1;
    /* if the source clock is input from the GPIO, then we're in the slave mode */
    if (clk_src != PARLIO_CLK_SRC_EXTERNAL) {
        ESP_RETURN_ON_FALSE(config->clk_freq_hz, ESP_ERR_INVALID_ARG, TAG, "clock frequency not set");
        /* get the internal clock source frequency */
        esp_clk_tree_src_get_freq_hz(clk_src, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &periph_src_clk_hz);
        /* set clock division, round up */
        div = (periph_src_clk_hz + config->clk_freq_hz - 1) / config->clk_freq_hz;
    } else {
        periph_src_clk_hz = config->clk_freq_hz;
    }

#if CONFIG_PM_ENABLE
    if (clk_src != PARLIO_CLK_SRC_EXTERNAL) {
        /* XTAL and PLL clock source will be turned off in light sleep, so we need to create a NO_LIGHT_SLEEP lock */
        sprintf(rx_unit->pm_lock_name, "parlio_rx_%d_%d", rx_unit->group->group_id, rx_unit->unit_id); // e.g. parlio_rx_0_0
        esp_err_t ret  = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, rx_unit->pm_lock_name, &rx_unit->pm_lock);
        ESP_RETURN_ON_ERROR(ret, TAG, "create NO_LIGHT_SLEEP lock failed");
    }
#endif

    /* Set clock configuration */
    parlio_ll_rx_set_clock_source(hal->regs, clk_src);
    parlio_ll_rx_set_clock_div(hal->regs, div);

    rx_unit->clk_src = clk_src;
    rx_unit->cfg.clk_freq_hz = periph_src_clk_hz / div;
    /* warning if precision lost due to division */
    if ((clk_src != PARLIO_CLK_SRC_EXTERNAL) &&
        (config->clk_freq_hz != rx_unit->cfg.clk_freq_hz )) {
        ESP_LOGW(TAG, "precision loss, real output frequency: %"PRIu32, rx_unit->cfg.clk_freq_hz );
    }

    return ESP_OK;
}

static esp_err_t s_parlio_destroy_rx_unit(parlio_rx_unit_handle_t rx_unit)
{
    /* Free the transaction queue */
    if (rx_unit->trans_que) {
        vQueueDeleteWithCaps(rx_unit->trans_que);
    }
    /* Free the mutex lock */
    if (rx_unit->mutex) {
        vSemaphoreDeleteWithCaps(rx_unit->mutex);
    }
    /* Free the transaction semaphore */
    if (rx_unit->trans_sem) {
        vSemaphoreDeleteWithCaps(rx_unit->trans_sem);
    }
    /* Free the power management lock */
    if (rx_unit->pm_lock) {
        ESP_RETURN_ON_ERROR(esp_pm_lock_delete(rx_unit->pm_lock), TAG, "delete pm lock failed");
    }
    /* Delete the GDMA channel */
    if (rx_unit->dma_chan) {
        ESP_RETURN_ON_ERROR(gdma_disconnect(rx_unit->dma_chan), TAG, "disconnect dma channel failed");
        ESP_RETURN_ON_ERROR(gdma_del_channel(rx_unit->dma_chan), TAG, "delete dma channel failed");
    }
    /* Free the DMA descriptors */
    if (rx_unit->dma_descs) {
        free(rx_unit->dma_descs);
    }
    /* Free the internal DMA buffer */
    if (rx_unit->dma_buf) {
        free(rx_unit->dma_buf);
    }
    /* Unregister the RX unit from the PARLIO group */
    if (rx_unit->group) {
        parlio_unregister_unit_from_group((parlio_unit_base_handle_t)rx_unit);
    }
    /* Free the RX unit */
    free(rx_unit);
    return ESP_OK;
}

esp_err_t parlio_new_rx_unit(const parlio_rx_unit_config_t *config, parlio_rx_unit_handle_t *ret_unit)
{
#if CONFIG_PARLIO_ENABLE_DEBUG_LOG
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif
    ESP_RETURN_ON_FALSE(config && ret_unit, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    /* Check the data width to be the the power of 2 */
    ESP_RETURN_ON_FALSE(__builtin_popcount(config->data_width) == 1, ESP_ERR_INVALID_ARG, TAG,
                        "data line number should be the power of 2 without counting valid signal");

    esp_err_t ret = ESP_OK;
    parlio_rx_unit_handle_t unit = NULL;

    /* Allocate unit memory */
    unit = heap_caps_calloc(1, sizeof(parlio_rx_unit_t), PARLIO_MEM_ALLOC_CAPS);
    ESP_GOTO_ON_FALSE(unit, ESP_ERR_NO_MEM, err, TAG, "no memory for rx unit");
    unit->dir = PARLIO_DIR_RX;
    unit->is_enabled = false;

    /* Initialize mutex lock */
    unit->mutex = xSemaphoreCreateMutexWithCaps(PARLIO_MEM_ALLOC_CAPS);
    /* Create transaction binary semaphore */
    unit->trans_sem = xSemaphoreCreateBinaryWithCaps(PARLIO_MEM_ALLOC_CAPS);
    ESP_GOTO_ON_FALSE(unit->trans_sem, ESP_ERR_NO_MEM, err, TAG, "no memory for transaction semaphore");
    xSemaphoreGive(unit->trans_sem);

    /* Create the transaction queue. Choose `parlio_rx_transaction_t` as the queue element instead of its pointer
     * Because the queue will do the copy to the element, no need to worry about the item in the queue will expire,
     * so that we don't have to allocate additional memory to store the transaction. */
    unit->trans_que = xQueueCreateWithCaps(config->trans_queue_depth, sizeof(parlio_rx_transaction_t), PARLIO_MEM_ALLOC_CAPS);
    ESP_GOTO_ON_FALSE(unit->trans_que, ESP_ERR_NO_MEM, err, TAG, "no memory for transaction queue");

    ESP_GOTO_ON_ERROR(s_parlio_rx_create_dma_descriptors(unit, config->max_recv_size), err, TAG, "create dma descriptor failed");
    /* Register and attach the rx unit to the group */
    ESP_GOTO_ON_ERROR(parlio_register_unit_to_group((parlio_unit_base_handle_t)unit), err, TAG, "failed to register the rx unit to the group");
    memcpy(&unit->cfg, config, sizeof(parlio_rx_unit_config_t));
    /* If not using external clock source, the internal clock is always a free running clock */
    if (config->clk_src != PARLIO_CLK_SRC_EXTERNAL) {
        unit->cfg.flags.free_clk = 1;
    }

    parlio_group_t *group = unit->group;
    parlio_hal_context_t *hal = &group->hal;
    /* Initialize GPIO */
    ESP_GOTO_ON_ERROR(s_parlio_rx_unit_set_gpio(unit, config), err, TAG, "failed to set GPIO");
    /* Install DMA service */
    ESP_GOTO_ON_ERROR(s_parlio_rx_unit_init_dma(unit), err, TAG, "install rx DMA failed");
    /* Reset RX module */
    parlio_ll_rx_reset_clock(hal->regs);
    parlio_ll_rx_reset_fifo(hal->regs);
    parlio_ll_rx_enable_clock(hal->regs, false);
    parlio_ll_rx_start(hal->regs, false);
    /* parlio_ll_clock_source_t and parlio_clock_source_t are binary compatible if the clock source is from internal */
    ESP_GOTO_ON_ERROR(s_parlio_select_periph_clock(unit, config), err, TAG, "set clock source failed");
    /* Set the data width */
    parlio_ll_rx_set_bus_width(hal->regs, config->data_width);
#if SOC_PARLIO_RX_CLK_SUPPORT_GATING
    parlio_ll_rx_enable_clock_gating(hal->regs, config->flags.clk_gate_en);
#endif  // SOC_PARLIO_RX_CLK_SUPPORT_GATING

    /* return RX unit handle */
    *ret_unit = unit;

    ESP_LOGD(TAG, "new rx unit(%d,%d) at %p, trans_queue_depth=%zu",
             group->group_id, unit->unit_id, (void *)unit, unit->cfg.trans_queue_depth);
    return ESP_OK;

err:
    if (unit) {
        s_parlio_destroy_rx_unit(unit);
    }
    return ret;
}

esp_err_t parlio_del_rx_unit(parlio_rx_unit_handle_t rx_unit)
{
    ESP_RETURN_ON_FALSE(rx_unit, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(!rx_unit->is_enabled, ESP_ERR_INVALID_STATE, TAG, "the unit has not disabled");

    ESP_LOGD(TAG, "del rx unit (%d, %d)", rx_unit->group->group_id, rx_unit->unit_id);
    return s_parlio_destroy_rx_unit(rx_unit);
}

esp_err_t parlio_rx_unit_enable(parlio_rx_unit_handle_t rx_unit, bool reset_queue)
{
    ESP_RETURN_ON_FALSE(rx_unit, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(!rx_unit->is_enabled, ESP_ERR_INVALID_STATE, TAG, "the unit has enabled or running");
    rx_unit->is_enabled = true;

    parlio_hal_context_t *hal = &rx_unit->group->hal;

    xSemaphoreTake(rx_unit->mutex, portMAX_DELAY);
    /* Acquire the power management lock incase */
    if (rx_unit->pm_lock) {
        esp_pm_lock_acquire(rx_unit->pm_lock);
    }

    /* For non-free running clock, the unit can't stop once enabled, otherwise the data alignment will go wrong */
    if (!rx_unit->cfg.flags.free_clk) {
        parlio_ll_rx_reset_fifo(hal->regs);
        parlio_ll_rx_start(hal->regs, true);
        parlio_ll_rx_enable_clock(hal->regs, true);
    }

    /* Check if we need to start a pending transaction */
    parlio_rx_transaction_t trans;
    if (reset_queue) {
        xQueueReset(rx_unit->trans_que);
        xSemaphoreGive(rx_unit->trans_sem);
    } else if (xQueueReceive(rx_unit->trans_que, &trans, 0) == pdTRUE) {
        xSemaphoreTake(rx_unit->trans_sem, 0);
        if (rx_unit->cfg.flags.free_clk) {
            parlio_ll_rx_enable_clock(hal->regs, false);
        }
        s_parlio_set_delimiter_config(rx_unit, trans.delimiter);
        s_parlio_mount_transaction_buffer(rx_unit, &rx_unit->curr_trans);
        gdma_start(rx_unit->dma_chan, (intptr_t)rx_unit->curr_desc);
        if (rx_unit->cfg.flags.free_clk) {
            parlio_ll_rx_start(hal->regs, true);
            parlio_ll_rx_enable_clock(hal->regs, true);
        }
    }
    xSemaphoreGive(rx_unit->mutex);

    return ESP_OK;
}

esp_err_t parlio_rx_unit_disable(parlio_rx_unit_handle_t rx_unit)
{
    ESP_RETURN_ON_FALSE(rx_unit, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(rx_unit->is_enabled, ESP_ERR_INVALID_STATE, TAG, "the unit has disabled");
    rx_unit->is_enabled = false;

    /* stop the RX engine */
    parlio_hal_context_t *hal = &rx_unit->group->hal;
    xSemaphoreTake(rx_unit->mutex, portMAX_DELAY);
    gdma_stop(rx_unit->dma_chan);
    parlio_ll_rx_enable_clock(hal->regs, false);
    parlio_ll_rx_start(hal->regs, false);
    if (rx_unit->curr_trans.delimiter) {
        rx_unit->curr_trans.delimiter->under_using = false;
    }

    /* For continuous receiving, free the temporary buffer and stop the DMA */
    if (rx_unit->dma_buf) {
        free(rx_unit->dma_buf);
        rx_unit->dma_buf = NULL;
    }
    /* release power management lock */
    if (rx_unit->pm_lock) {
        esp_pm_lock_release(rx_unit->pm_lock);
    }
    /* Erase the current transaction */
    memset(&rx_unit->curr_trans, 0, sizeof(parlio_rx_transaction_t));
    xSemaphoreGive(rx_unit->mutex);

    return ESP_OK;
}

esp_err_t parlio_new_rx_level_delimiter(const parlio_rx_level_delimiter_config_t *config,
                                        parlio_rx_delimiter_handle_t *ret_delimiter)
{
    ESP_RETURN_ON_FALSE(config && ret_delimiter, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    /* Validation signal line must specified for level delimiter */
    ESP_RETURN_ON_FALSE(config->valid_sig_line_id < PARLIO_RX_UNIT_MAX_DATA_WIDTH,
                        ESP_ERR_INVALID_ARG, TAG, "no valid signal line specified");

    parlio_rx_delimiter_handle_t delimiter = NULL;

    /* Allocate memory for the delimiter */
    delimiter = (parlio_rx_delimiter_handle_t)heap_caps_calloc(1, sizeof(parlio_rx_delimiter_t), PARLIO_MEM_ALLOC_CAPS);
    ESP_RETURN_ON_FALSE(delimiter, ESP_ERR_NO_MEM, TAG, "no memory for rx delimiter");

    /* Assign configuration for the level delimiter */
    delimiter->mode = PARLIO_RX_LEVEL_MODE;
    delimiter->valid_sig_line_id = config->valid_sig_line_id;
    delimiter->sample_edge = config->sample_edge;
    delimiter->bit_pack_order = config->bit_pack_order;
    delimiter->eof_data_len = config->eof_data_len;
    delimiter->timeout_ticks = config->timeout_ticks;
    delimiter->flags.active_level = config->flags.active_level;

    *ret_delimiter = delimiter;

    return ESP_OK;
}

esp_err_t parlio_new_rx_pulse_delimiter(const parlio_rx_pulse_delimiter_config_t *config,
                                        parlio_rx_delimiter_handle_t *ret_delimiter)
{
    ESP_RETURN_ON_FALSE(config && ret_delimiter, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    /* Validation signal line must specified for pulse delimiter */
    ESP_RETURN_ON_FALSE(config->valid_sig_line_id < PARLIO_RX_UNIT_MAX_DATA_WIDTH,
                        ESP_ERR_INVALID_ARG, TAG, "no valid signal line specified");
    /* Guarantee there is an end symbol, end by length or pulse */
    ESP_RETURN_ON_FALSE(config->eof_data_len || config->flags.has_end_pulse,
                        ESP_ERR_INVALID_ARG, TAG, "Either eof_data_len or has_end_pulse should be set");
    /* If end by length, the maximum length is limited */
    ESP_RETURN_ON_FALSE(config->eof_data_len <= PARLIO_LL_RX_MAX_BYTES_PER_FRAME, ESP_ERR_INVALID_ARG,
                        TAG, "EOF data length exceed the max value %d", PARLIO_LL_RX_MAX_BYTES_PER_FRAME);

    parlio_rx_delimiter_handle_t delimiter = NULL;

    /* Allocate memory for the delimiter */
    delimiter = (parlio_rx_delimiter_handle_t)heap_caps_calloc(1, sizeof(parlio_rx_delimiter_t), PARLIO_MEM_ALLOC_CAPS);
    ESP_RETURN_ON_FALSE(delimiter, ESP_ERR_NO_MEM, TAG, "no memory for rx delimiter");

    /* Assign configuration for the pulse delimiter */
    delimiter->mode = PARLIO_RX_PULSE_MODE;
    delimiter->valid_sig_line_id = config->valid_sig_line_id;
    delimiter->sample_edge = config->sample_edge;
    delimiter->bit_pack_order = config->bit_pack_order;
    delimiter->eof_data_len = config->eof_data_len;
    delimiter->timeout_ticks = config->timeout_ticks;
    delimiter->flags.start_bit_included = config->flags.start_bit_included;
    delimiter->flags.has_end_pulse = config->flags.has_end_pulse;
    delimiter->flags.end_bit_included = config->flags.end_bit_included;
    delimiter->flags.pulse_invert = config->flags.pulse_invert;

    *ret_delimiter = delimiter;

    return ESP_OK;
}

esp_err_t parlio_new_rx_soft_delimiter(const parlio_rx_soft_delimiter_config_t *config,
                                       parlio_rx_delimiter_handle_t *ret_delimiter)
{
    ESP_RETURN_ON_FALSE(config && ret_delimiter, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    /* The soft delimiter can only end by length, EOF length should be within range (0, PARLIO_LL_RX_MAX_BYTES_PER_FRAME] */
    ESP_RETURN_ON_FALSE(config->eof_data_len > 0 && config->eof_data_len <= PARLIO_LL_RX_MAX_BYTES_PER_FRAME, ESP_ERR_INVALID_ARG,
                        TAG, "EOF data length is 0 or exceed the max value %d", PARLIO_LL_RX_MAX_BYTES_PER_FRAME);

    parlio_rx_delimiter_handle_t delimiter = NULL;

    /* Allocate memory for the delimiter */
    delimiter = (parlio_rx_delimiter_handle_t)heap_caps_calloc(1, sizeof(parlio_rx_delimiter_t), PARLIO_MEM_ALLOC_CAPS);
    ESP_RETURN_ON_FALSE(delimiter, ESP_ERR_NO_MEM, TAG, "no memory for rx delimiter");

    /* Assign configuration for the soft delimiter */
    delimiter->mode = PARLIO_RX_SOFT_MODE;
    delimiter->under_using = false;
    delimiter->sample_edge = config->sample_edge;
    delimiter->bit_pack_order = config->bit_pack_order;
    delimiter->eof_data_len = config->eof_data_len;
    delimiter->timeout_ticks = config->timeout_ticks;

    *ret_delimiter = delimiter;

    return ESP_OK;
}

esp_err_t parlio_rx_soft_delimiter_start_stop(parlio_rx_unit_handle_t rx_unit, parlio_rx_delimiter_handle_t delimiter, bool start_stop)
{
    ESP_RETURN_ON_FALSE(rx_unit && delimiter, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(delimiter->mode == PARLIO_RX_SOFT_MODE, ESP_ERR_INVALID_ARG, TAG, "The delimiter is not soft delimiter");
    ESP_RETURN_ON_FALSE(rx_unit->is_enabled, ESP_ERR_INVALID_STATE, TAG, "the unit has not enabled");

    xSemaphoreTake(rx_unit->mutex, portMAX_DELAY);
    parlio_hal_context_t *hal = &(rx_unit->group->hal);
    parlio_ll_rx_start_soft_recv(hal->regs, start_stop);
    xSemaphoreGive(rx_unit->mutex);
    return ESP_OK;
}

esp_err_t parlio_del_rx_delimiter(parlio_rx_delimiter_handle_t delimiter)
{
    ESP_RETURN_ON_FALSE(delimiter, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(!delimiter->under_using, ESP_ERR_INVALID_STATE, TAG, "the delimiter is under using");
    free(delimiter);
    return ESP_OK;
}

static esp_err_t s_parlio_rx_unit_do_transaction(parlio_rx_unit_handle_t rx_unit, parlio_rx_transaction_t *trans)
{
    if (!rx_unit->curr_trans.delimiter) {
        xSemaphoreTake(rx_unit->trans_sem, 0);
        if (rx_unit->cfg.flags.free_clk) {
            parlio_ll_rx_enable_clock(rx_unit->group->hal.regs, false);
        }
        if (trans->delimiter != rx_unit->curr_trans.delimiter) {
            s_parlio_set_delimiter_config(rx_unit, trans->delimiter);
        }
        s_parlio_mount_transaction_buffer(rx_unit, trans);
        gdma_start(rx_unit->dma_chan, (intptr_t)rx_unit->curr_desc);
        if (rx_unit->cfg.flags.free_clk) {
            parlio_ll_rx_start(rx_unit->group->hal.regs, true);
            parlio_ll_rx_enable_clock(rx_unit->group->hal.regs, true);
        }
    } else { // Otherwise send to the queue
        /* Send the transaction to the queue */
        ESP_RETURN_ON_FALSE(xQueueSend(rx_unit->trans_que, trans, 0) == pdTRUE,
                            ESP_ERR_INVALID_STATE, TAG, "transaction queue is full, failed to send transaction to the queue");
    }
    return ESP_OK;
}

esp_err_t parlio_rx_unit_receive(parlio_rx_unit_handle_t rx_unit,
                                 void *payload,
                                 size_t payload_size,
                                 const parlio_receive_config_t* recv_cfg)
{
    ESP_RETURN_ON_FALSE(rx_unit && payload && recv_cfg, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(recv_cfg->delimiter, ESP_ERR_INVALID_ARG, TAG, "no delimiter specified");
    ESP_RETURN_ON_FALSE(payload_size <= rx_unit->max_recv_size, ESP_ERR_INVALID_ARG, TAG, "trans length too large");
#if CONFIG_GDMA_ISR_IRAM_SAFE
    ESP_RETURN_ON_FALSE(esp_ptr_internal(payload), ESP_ERR_INVALID_ARG, TAG, "payload not in internal RAM");
#endif
    if (recv_cfg->delimiter->eof_data_len) {
        ESP_RETURN_ON_FALSE(payload_size >= recv_cfg->delimiter->eof_data_len, ESP_ERR_INVALID_ARG,
                            TAG, "payload size should be greater than eof_data_len");
    }
    if (recv_cfg->delimiter->mode != PARLIO_RX_SOFT_MODE) {
        ESP_RETURN_ON_FALSE(rx_unit->cfg.valid_gpio_num >= 0, ESP_ERR_INVALID_ARG, TAG, "The validate gpio of this unit is not set");
        /* Check if the valid_sig_line_id is equal or greater than data width, otherwise valid_sig_line_id is conflict with data signal.
         * Specifically, level or pulse delimiter requires one data line as valid signal, so these two delimiters can't support PARLIO_RX_UNIT_MAX_DATA_WIDTH */
        ESP_RETURN_ON_FALSE(recv_cfg->delimiter->valid_sig_line_id >= rx_unit->cfg.data_width,
            ESP_ERR_INVALID_ARG, TAG, "the valid_sig_line_id of this delimiter is conflict with rx unit data width");
        /* Assign the signal here to ensure iram safe */
        recv_cfg->delimiter->valid_sig = parlio_periph_signals.groups[rx_unit->group->group_id].
                                         rx_units[rx_unit->unit_id].
                                         data_sigs[recv_cfg->delimiter->valid_sig_line_id];
    }
    void *p_buffer = payload;

    /* Create the internal DMA buffer for the infinite transaction if indirect_mount is set */
    if (recv_cfg->flags.is_infinite && recv_cfg->flags.indirect_mount) {
        /* Allocate the internal DMA buffer to store the data temporary */
        rx_unit->dma_buf = heap_caps_calloc(1, payload_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        ESP_RETURN_ON_FALSE(rx_unit->dma_buf, ESP_ERR_NO_MEM, TAG, "No memory for the internal DMA buffer");
        /* Use the internal DMA buffer so that the user buffer can always be available */
        p_buffer = rx_unit->dma_buf;
    }

    /* Create the transaction */
    parlio_rx_transaction_t transaction = {
        .delimiter = recv_cfg->delimiter,
        .payload = p_buffer,
        .size = payload_size,
        .recv_bytes = 0,
        .flags.infinite = recv_cfg->flags.is_infinite,
    };
    rx_unit->usr_recv_buf = payload;

    xSemaphoreTake(rx_unit->mutex, portMAX_DELAY);
    esp_err_t ret = s_parlio_rx_unit_do_transaction(rx_unit, &transaction);
    xSemaphoreGive(rx_unit->mutex);
    return ret;
}

esp_err_t parlio_rx_unit_wait_all_done(parlio_rx_unit_handle_t rx_unit, int timeout_ms)
{
    ESP_RETURN_ON_FALSE(rx_unit, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    xSemaphoreTake(rx_unit->mutex, portMAX_DELAY);
    TickType_t ticks = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    /* Waiting for the all transaction done signal */
    if (xSemaphoreTake(rx_unit->trans_sem, ticks) == pdFALSE) {
        xSemaphoreGive(rx_unit->mutex);
        return ESP_ERR_TIMEOUT;
    }
    /* Put back the signal */
    xSemaphoreGive(rx_unit->trans_sem);
    xSemaphoreGive(rx_unit->mutex);

    return ESP_OK;
}

esp_err_t parlio_rx_unit_register_event_callbacks(parlio_rx_unit_handle_t rx_unit, const parlio_rx_event_callbacks_t *cbs, void *user_data)
{
    ESP_RETURN_ON_FALSE(rx_unit, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(!rx_unit->is_enabled, ESP_ERR_INVALID_STATE, TAG, "the unit has enabled or running");

    xSemaphoreTake(rx_unit->mutex, portMAX_DELAY);
    memcpy(&rx_unit->cbs, cbs, sizeof(parlio_rx_event_callbacks_t));
    rx_unit->user_data = user_data;
    xSemaphoreGive(rx_unit->mutex);

    return ESP_OK;
}
