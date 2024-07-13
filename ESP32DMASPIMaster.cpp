#include "ESP32DMASPIMaster.h"

#include <Arduino.h>

ARDUINO_ESP32_DMA_SPI_NAMESPACE_BEGIN

QueueHandle_t Master::s_trans_queue_handle = NULL;
QueueHandle_t Master::s_trans_result_handle = NULL;
QueueHandle_t Master::s_trans_error_handle = NULL;
QueueHandle_t Master::s_in_flight_mailbox_handle = NULL;

void IRAM_ATTR spi_master_pre_cb(spi_transaction_t *trans)
{
    spi_master_cb_user_context_t *user_ctx = static_cast<spi_master_cb_user_context_t *>(trans->user);
    if (user_ctx->pre.user_cb)
    {
        user_ctx->pre.user_cb(trans, user_ctx->pre.user_arg);
    }
}

void IRAM_ATTR spi_master_post_cb(spi_transaction_t *trans)
{
    spi_master_cb_user_context_t *user_ctx = static_cast<spi_master_cb_user_context_t *>(trans->user);
    if (user_ctx->post.user_cb)
    {
        user_ctx->post.user_cb(trans, user_ctx->post.user_arg);
    }
}

void spi_master_task(void *arg)
{
    DMALOG_D(TAG, "spi_master_task started");

    spi_master_context_t *ctx = static_cast<spi_master_context_t *>(arg);

    // initialize spi bus
    DMALOG_D(TAG, "initialize spi bus");
    esp_err_t err = spi_bus_initialize(ctx->host, &ctx->bus_cfg, ctx->dma_chan);
    assert(err == ESP_OK);

    // add spi device
    spi_device_handle_t device_handle;
    DMALOG_D(TAG, "add spi device");
    err = spi_bus_add_device(ctx->host, &ctx->if_cfg, &device_handle);
    assert(err == ESP_OK);

    // initialize queues
    DMALOG_D(TAG, "initialize queues");
    Master::s_trans_queue_handle = xQueueCreate(1, sizeof(spi_transaction_context_t));
    assert(Master::s_trans_queue_handle != NULL);
    Master::s_trans_result_handle = xQueueCreate(ctx->if_cfg.queue_size, sizeof(size_t));
    assert(Master::s_trans_result_handle != NULL);
    Master::s_trans_error_handle = xQueueCreate(ctx->if_cfg.queue_size, sizeof(esp_err_t));
    assert(Master::s_trans_error_handle != NULL);
    Master::s_in_flight_mailbox_handle = xQueueCreate(1, sizeof(size_t));
    assert(Master::s_in_flight_mailbox_handle != NULL);

    DMALOG_D(TAG, "Pointers allocated are: \n\r\tQueue Handle: %p, \n\r\tResult Handle: %p, \n\r\tError Handle: %p, \n\r\tIn Flight Handle %p",
             Master::s_trans_queue_handle,
             Master::s_trans_queue_handle,
             Master::s_trans_error_handle,
             Master::s_in_flight_mailbox_handle);

    // spi task
    DMALOG_D(TAG, "spi_master_task start loop");
    while (true)
    {
        spi_transaction_context_t trans_ctx;
        DMALOG_D(TAG, "wait for new transaction request");
        if (Master::s_trans_queue_handle == NULL)
        {
            DMALOG_D(TAG, "Queue Handle is NULL");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }
        if (xQueueReceive(Master::s_trans_queue_handle, &trans_ctx, RECV_TRANS_QUEUE_TIMEOUT_TICKS))
        {
            DMALOG_D(TAG, "new transaction request received");
            // update in-flight count
            assert(trans_ctx.trans_ext != nullptr);
            assert(trans_ctx.size <= ctx->if_cfg.queue_size);
            DMALOG_D(TAG, "update in-flight count");
            xQueueOverwrite(Master::s_in_flight_mailbox_handle, &trans_ctx.size);

            // execute new transaction if transaction request received from main task
            DMALOG_D(TAG, "new transaction request received (size = %u)", trans_ctx.size);
            std::vector<esp_err_t> errs;
            errs.reserve(trans_ctx.size);
            for (size_t i = 0; i < trans_ctx.size; ++i)
            {
                spi_transaction_t *trans = (spi_transaction_t *)(&trans_ctx.trans_ext[i]);
                esp_err_t err = spi_device_queue_trans(device_handle, trans, trans_ctx.timeout_ticks);
                if (err != ESP_OK)
                {
                    DMALOG_D(TAG, "failed to execute spi_device_queue_trans(): 0x%X", err);
                }
                errs.push_back(err);
            }

            // wait for the completion of all of the queued transactions
            // reset result/error queue first
            xQueueReset(Master::s_trans_result_handle);
            xQueueReset(Master::s_trans_error_handle);
            for (size_t i = 0; i < trans_ctx.size; ++i)
            {
                // wait for completion of next transaction
                size_t num_received_bytes = 0;
                if (errs[i] == ESP_OK)
                {
                    spi_transaction_t *rtrans;
                    esp_err_t err = spi_device_get_trans_result(device_handle, &rtrans, trans_ctx.timeout_ticks);
                    if (err != ESP_OK)
                    {
                        DMALOG_D(TAG, "failed to execute spi_device_get_trans_result(): 0x%X", err);
                    }
                    else
                    {
                        num_received_bytes = rtrans->rxlength / 8; // bit -> byte
                        DMALOG_D(TAG, "transaction complete: %d bits (%d bytes) received", rtrans->rxlength, num_received_bytes);
                    }
                }
                else
                {
                    DMALOG_D(TAG, "skip spi_device_get_trans_result() because queue was failed: index = %u", i);
                }

                // send the received bytes back to main task
                if (!xQueueSend(Master::s_trans_result_handle, &num_received_bytes, SEND_TRANS_RESULT_TIMEOUT_TICKS))
                {
                    DMALOG_D(TAG, "failed to send a number of received bytes to main task: %d", err);
                }
                // send the transaction error back to main task
                if (!xQueueSend(Master::s_trans_error_handle, &errs[i], SEND_TRANS_ERROR_TIMEOUT_TICKS))
                {
                    DMALOG_D(TAG, "failed to send a transaction error to main task: %d", err);
                }

                // update in-flight count
                const size_t num_rest_in_flight = trans_ctx.size - (i + 1);
                xQueueOverwrite(Master::s_in_flight_mailbox_handle, &num_rest_in_flight);
            }

            // should be deleted because the ownership is moved from main task
            delete[] trans_ctx.trans_ext;

            DMALOG_D(TAG, "all requested transactions completed");
        }

        // terminate task if requested
        if (xTaskNotifyWait(0, 0, NULL, 0) == pdTRUE)
        {
            DMALOG_D(TAG, "Terminate Requested!");
            break;
        }
    }

    DMALOG_D(TAG, "terminate spi task as requested by the main task");

    vQueueDelete(Master::s_in_flight_mailbox_handle);
    vQueueDelete(Master::s_trans_result_handle);
    vQueueDelete(Master::s_trans_error_handle);
    vQueueDelete(Master::s_trans_queue_handle);

    spi_bus_remove_device(device_handle);
    spi_bus_free(ctx->host);

    xTaskNotifyGive(ctx->main_task_handle);
    DMALOG_D(TAG, "spi_master_task finished");

    vTaskDelete(NULL);
}

ARDUINO_ESP32_DMA_SPI_NAMESPACE_END
