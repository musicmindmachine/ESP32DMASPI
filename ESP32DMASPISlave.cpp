#include "ESP32DMASPISlave.h"

#include <Arduino.h>

ARDUINO_ESP32_DMA_SPI_NAMESPACE_BEGIN

void IRAM_ATTR spi_slave_post_setup_cb(spi_slave_transaction_t *trans)
{
    spi_slave_cb_user_context_t *user_ctx = static_cast<spi_slave_cb_user_context_t *>(trans->user);
    if (user_ctx->post_setup.user_cb)
    {
        user_ctx->post_setup.user_cb(trans, user_ctx->post_setup.user_arg);
    }
}

void IRAM_ATTR spi_slave_post_trans_cb(spi_slave_transaction_t *trans)
{
    spi_slave_cb_user_context_t *user_ctx = static_cast<spi_slave_cb_user_context_t *>(trans->user);
    if (user_ctx->post_trans.user_cb)
    {
        user_ctx->post_trans.user_cb(trans, user_ctx->post_trans.user_arg);
    }
}

void spi_slave_task(void *arg)
{
    ESP_LOGD(TAG, "spi_slave_task start");

    spi_slave_context_t *ctx = static_cast<spi_slave_context_t *>(arg);

    // initialize spi slave
    esp_err_t err = spi_slave_initialize(ctx->host, &ctx->bus_cfg, &ctx->if_cfg, ctx->dma_chan);
    assert(err == ESP_OK);

    // initialize queues
    s_trans_queue_handle = xQueueCreate(1, sizeof(spi_transaction_context_t));
    assert(s_trans_queue_handle != NULL);
    s_trans_result_handle = xQueueCreate(ctx->if_cfg.queue_size, sizeof(size_t));
    assert(s_trans_result_handle != NULL);
    s_trans_error_handle = xQueueCreate(ctx->if_cfg.queue_size, sizeof(esp_err_t));
    assert(s_trans_error_handle != NULL);
    s_in_flight_mailbox_handle = xQueueCreate(1, sizeof(size_t));
    assert(s_in_flight_mailbox_handle != NULL);

    // spi task
    while (true)
    {
        spi_transaction_context_t trans_ctx;
        if (xQueueReceive(s_trans_queue_handle, &trans_ctx, RECV_TRANS_QUEUE_TIMEOUT_TICKS))
        {
            // update in-flight count
            assert(trans_ctx.trans != nullptr);
            assert(trans_ctx.size <= ctx->if_cfg.queue_size);
            xQueueOverwrite(s_in_flight_mailbox_handle, &trans_ctx.size);

            // execute new transaction if transaction request received from main task
            ESP_LOGD(TAG, "new transaction request received (size = %u)", trans_ctx.size);
            std::vector<esp_err_t> errs;
            errs.reserve(trans_ctx.size);
            for (size_t i = 0; i < trans_ctx.size; ++i)
            {
                spi_slave_transaction_t *trans = &trans_ctx.trans[i];
                esp_err_t err = spi_slave_queue_trans(ctx->host, trans, trans_ctx.timeout_ticks);
                if (err != ESP_OK)
                {
                    ESP_LOGE(TAG, "failed to execute spi_slave_queue_trans(): 0x%X", err);
                }
                errs.push_back(err);
            }

            // wait for the completion of all of the queued transactions
            // reset result/error queue first
            xQueueReset(s_trans_result_handle);
            xQueueReset(s_trans_error_handle);
            for (size_t i = 0; i < trans_ctx.size; ++i)
            {
                // wait for completion of next transaction
                size_t num_received_bytes = 0;
                if (errs[i] == ESP_OK)
                {
                    spi_slave_transaction_t *rtrans;
                    esp_err_t err = spi_slave_get_trans_result(ctx->host, &rtrans, trans_ctx.timeout_ticks);
                    if (err != ESP_OK)
                    {
                        ESP_LOGE(TAG, "failed to execute spi_slave_get_trans_result(): 0x%X", err);
                    }
                    else
                    {
                        num_received_bytes = rtrans->trans_len / 8; // bit -> byte
                        ESP_LOGD(TAG, "transaction complete: %d bits (%d bytes) received", rtrans->trans_len, num_received_bytes);
                    }
                }
                else
                {
                    ESP_LOGE(TAG, "skip spi_slave_get_trans_result() because queue was failed: index = %u", i);
                }

                // send the received bytes back to main task
                if (!xQueueSend(s_trans_result_handle, &num_received_bytes, SEND_TRANS_RESULT_TIMEOUT_TICKS))
                {
                    ESP_LOGE(TAG, "failed to send a number of received bytes to main task: %d", err);
                }
                // send the transaction error back to main task
                if (!xQueueSend(s_trans_error_handle, &errs[i], SEND_TRANS_ERROR_TIMEOUT_TICKS))
                {
                    ESP_LOGE(TAG, "failed to send a transaction error to main task: %d", err);
                }

                // update in-flight count
                const size_t num_rest_in_flight = trans_ctx.size - (i + 1);
                xQueueOverwrite(s_in_flight_mailbox_handle, &num_rest_in_flight);
            }

            // should be deleted because the ownership is moved from main task
            delete[] trans_ctx.trans;

            ESP_LOGD(TAG, "all requested transactions completed");
        }

        // terminate task if requested
        if (xTaskNotifyWait(0, 0, NULL, 0) == pdTRUE)
        {
            break;
        }
    }

    ESP_LOGD(TAG, "terminate spi task as requested by the main task");

    vQueueDelete(s_in_flight_mailbox_handle);
    vQueueDelete(s_trans_result_handle);
    vQueueDelete(s_trans_error_handle);
    vQueueDelete(s_trans_queue_handle);

    spi_slave_free(ctx->host);

    xTaskNotifyGive(ctx->main_task_handle);
    ESP_LOGD(TAG, "spi_slave_task finished");

    vTaskDelete(NULL);
}

ARDUINO_ESP32_DMA_SPI_NAMESPACE_END
