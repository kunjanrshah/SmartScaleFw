
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_now.h"
#include "esp_crc.h"
#include "espnow_example.h"
#include <sys/param.h>
#include "freertos/task.h"
#include "protocol_examples_common.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "freertos/queue.h"
#include "driver/gpio.h"
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_err.h"
#include "esp_spiffs.h"
#include "freertos/event_groups.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "nvs.h"
#include <unistd.h>
#include "esp_timer.h"
#include "esp_sleep.h"
#include "sdkconfig.h"
#include "cJSON.h"

static const char *TAG = "espnow_example";
static xQueueHandle s_example_espnow_queue;
static uint8_t device_mac_addr[6] = {0};
static uint8_t s_example_broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint8_t rx_cmd[] = {0x0, 0x00}; //{0};
static uint8_t *store_status = {0};
static int n_status=21;
static bool is_internet_connected=false;
static bool save_status = false;
static void example_espnow_deinit(example_espnow_send_param_t *send_param);

#include "utils.h"
#include "tcp_server.h"
#include "storage_init.h"
#include "gpio_task.h"
#include "ota_init.h"
#include "mqtt_server.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"


esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");            
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            break;
    }
    return ESP_OK;
}

/* ESPNOW sending or receiving callback function is called in WiFi task.
 * Users should not do lengthy operations from this task. Instead, post
 * necessary data to a queue and handle it from a lower priority task. */
static void example_espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    example_espnow_event_t evt;
    example_espnow_event_send_cb_t *send_cb = &evt.info.send_cb;

    if (mac_addr == NULL)
    {
        ESP_LOGE(TAG, "Send cb arg error");
        return;
    }

    evt.id = EXAMPLE_ESPNOW_SEND_CB;
    memcpy(send_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    send_cb->status = status;
    if (xQueueSend(s_example_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send send queue fail");
    }
}

static void example_espnow_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len)
{
    example_espnow_event_t evt;
    example_espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;

    if (mac_addr == NULL || data == NULL || len <= 0)
    {
        ESP_LOGE(TAG, "Receive cb arg error");
        return;
    }

    evt.id = EXAMPLE_ESPNOW_RECV_CB;
    memcpy(recv_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    recv_cb->data = malloc(len);
    if (recv_cb->data == NULL)
    {
        ESP_LOGE(TAG, "Malloc receive data fail");
        return;
    }
    memcpy(recv_cb->data, data, len);
    recv_cb->data_len = len;
    if (xQueueSend(s_example_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send receive queue fail");
        free(recv_cb->data);
    }
}

/* Parse received ESPNOW data. */
int example_espnow_data_parse(example_espnow_send_param_t *send_param, uint8_t *mac_addr, uint8_t *data, uint16_t data_len)
{
    example_espnow_data_t *buf = (example_espnow_data_t *)data;
    uint16_t crc, crc_cal = 0;

    ESP_LOGI(TAG, "Receive  data_len: %d, sizeof: %d", data_len, sizeof(example_espnow_data_t));
    if (data_len < sizeof(example_espnow_data_t))
    {
        ESP_LOGE(TAG, "Receive ESPNOW data too short, len:%d", data_len);
        return -1;
    }

    n_status = (int)(sizeof(send_param->seq_status) / sizeof(send_param->seq_status[0]));
    if (buf->seq_status != NULL)
    {
        for (int i = 0; i < n_status; i++)
        {
            send_param->seq_status[i] = buf->seq_status[i];
          //  ESP_LOGI(TAG, "Receiving:  seq_status: %d", buf->seq_status[i]);
        }
    }
    else
    {
        ESP_LOGI(TAG, "Receiving:  seq_status:  NULL");
    }

    if (buf->seq_cmd != NULL)
    {
        for (int i = 0; i < 2; i++)
        {
            send_param->seq_cmd[i] = buf->seq_cmd[i];
            ESP_LOGI(TAG, "Receiving:  seq_cmd: %d", buf->seq_cmd[i]);
        }
    }
    else
    {
        ESP_LOGI(TAG, "Receiving:  seq_cmd: NULL");
    }

    crc = buf->crc;
    buf->crc = 0;
    crc_cal = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, data_len);

    if (crc_cal == crc)
    {
        return 1;
    }

    return 0;
}

// Create array
cJSON *Create_array(cJSON **objects,int array_num)
{
	cJSON *prev = 0;
	cJSON *root;
	root = cJSON_CreateArray();
	for (int i=0;i<array_num;i++) {
		if (!i)	{
			root->child=objects[i];
		} else {
			prev->next=objects[i];
			objects[i]->prev=prev;
		}
		prev=objects[i];
	}
	return root;
}

// Create array
cJSON *Create_array1(cJSON **objects1,int array_num)
{
	cJSON *prev = 0;
	cJSON *root;
	root = cJSON_CreateArray();
	for (int i=0;i<array_num;i++) {
		if (!i)	{
			root->child=objects1[i];
		} else {
			prev->next=objects1[i];
			objects1[i]->prev=prev;
		}
		prev=objects1[i];
	}
	return root;
}


static void http_store_unit_task(void *pvParameters)
{
    uint8_t *unit_param  = (uint8_t *)pvParameters;
    cJSON *root,*array;
    root = cJSON_CreateObject();
	array = cJSON_CreateArray();

    int n=0;
    for (int i = 0; i < 8; i=i+2)
    {
       if(unit_param[i]!=0){
           n++;
       } 
    }
    ESP_LOGE(TAG, "n of objects: %d", n);
    cJSON *objects[n];
    for(int i=0;i<n;i++) {
		objects[i] = cJSON_CreateObject();
	}
    
     array = Create_array(objects, n);
    
    for (int i=0,j=0;j<n;j++,i=i+2) {
		cJSON_AddNumberToObject(objects[j], "device_id", unit_param[i]);
		cJSON_AddNumberToObject(objects[j], "unit", unit_param[i+1]);
	}
    cJSON_AddItemToObject(root, "units", array);
	const char *my_json_string = cJSON_Print(root);
	ESP_LOGI(TAG, "my_json_string\n%s",my_json_string);
	cJSON_Delete(root);

    esp_http_client_config_t config = {
        .url = "http://www.superbinstruments.com/units_data.php",
        .method= HTTP_METHOD_POST,
        .event_handler = _http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

//  {"units":[{"device_id":1,"unit": 11}, {"device_id":2,"unit": 22}, {"device_id":3,"unit": 34}, {"device_id":4,"unit": 45}] }
    
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, my_json_string, strlen(my_json_string));
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %lld",
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    vTaskDelete(NULL);
}

static void http_store_keys_task(void *pvParameters)
{
   //  {"keys":[{"device_id":1,"switch":[{"switch_id": 1,"status": 0},{"switch_id": 2,"status": 0}]}, {"device_id":2,"switch":[{"switch_id": 1,"status": 1},{"switch_id": 2,"status": 1}]},{"device_id":3,"switch":[{"switch_id": 3,"status": 0}]}] } 
    uint8_t *keys_param  = (uint8_t *)pvParameters;
    cJSON *root,*array;
    root = cJSON_CreateObject();
	array = cJSON_CreateArray();

    cJSON *objects[4];
    for(int i=0;i<4;i++) {
		objects[i] = cJSON_CreateObject();
	}
    
    array = Create_array(objects, 4);
    
    for (int j=0,i=0;j<4;j++,i=i+9) {   
        if(keys_param[i] != 0){
        cJSON_AddNumberToObject(objects[j], "device_id", keys_param[i]);

        cJSON *objects1[4];
        for(int z=0;z<4;z++) {
            objects1[z] = cJSON_CreateObject();
        }
        cJSON *array1;
        array1 = cJSON_CreateArray();
        array1 = Create_array1(objects1, 4);

        for (int k=i+1,x=0;k<i+8;k=k+2,x++) {
            if(keys_param[k] != 0){
                cJSON_AddNumberToObject(objects1[x], "switch_id", keys_param[k]);
		        cJSON_AddNumberToObject(objects1[x], "status", keys_param[k+1]);
            }
	    }
         cJSON_AddItemToObject(objects[j], "switch",array1);
        }	
	 }

     cJSON_AddItemToObject(root, "keys", array);
	 const char *my_json_string = cJSON_Print(root);
     ESP_LOGI(TAG, "my_json_string\n%s",my_json_string);
	 cJSON_Delete(root);

    esp_http_client_config_t config = {
        .url = "http://www.superbinstruments.com/keys_data.php",
        .method= HTTP_METHOD_POST,
        .event_handler = _http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, my_json_string, strlen(my_json_string));
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %lld",
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    vTaskDelete(NULL);
}

/* Prepare ESPNOW data to be sent. */
void example_espnow_data_prepare(example_espnow_send_param_t *send_param)
{
    example_espnow_data_t *buf = (example_espnow_data_t *)send_param->buffer;

    assert(send_param->len >= sizeof(example_espnow_data_t));

    buf->crc = 0;
    int dev_id = 1;
    int count = 0;
    n_status = (int)(sizeof(send_param->seq_status) / sizeof(send_param->seq_status[0]));
    
    for (int i = 0; i < n_status; i++)
    {
        if (send_param->seq_status[i] == 0)
        {
            count++;
        }
    }

    if (count == n_status)
    {

        int connected = 0;
        if (IS_CONNECTED)
        {
            connected = setBit(connected, ESPNOW_DEVICE_ID);
        }
        else
        {
            connected = clearBit(connected, ESPNOW_DEVICE_ID);
        }
        buf->seq_status[0] = connected;

        for (int i = 1; i < n_status; i++)
        {
            if (dev_id == ESPNOW_DEVICE_ID)
            {
                buf->seq_status[i] = dev_id;
                buf->seq_status[++i] = 253;
                buf->seq_status[++i] = 254;
                buf->seq_status[++i] = ESPNOW_SWITCH;
                if (rx_cmd[0] == ESPNOW_DEVICE_ID)
                {
                    buf->seq_status[++i] = rx_cmd[1];
                    for (int j = 0; j < 2; j++)
                    {
                        rx_cmd[j] = 0;
                    }
                }
                else
                {
                    buf->seq_status[++i] = 0;
                }
            }
            else
            {
                buf->seq_status[i] = dev_id;
                buf->seq_status[++i] = 253;
                buf->seq_status[++i] = 254;
                buf->seq_status[++i] = 0;
                buf->seq_status[++i] = 0;
            }
            dev_id++;
        }
        for (int j = 0; j < 2; j++)
        {
            buf->seq_cmd[j] = rx_cmd[j];
            send_param->seq_cmd[j] = rx_cmd[j];
        }
    }
    else if (count != n_status)
    {
        for (int i = 0; i < n_status; i++)
        {
            buf->seq_status[i] = send_param->seq_status[i];
        }

        int connected = buf->seq_status[0];
        if (IS_CONNECTED)
        {
            connected = setBit(connected, ESPNOW_DEVICE_ID);
        }
        else
        {
            connected = clearBit(connected, ESPNOW_DEVICE_ID);
        }
        buf->seq_status[0] = connected;

        for (int i = 1; i < n_status; i = i + 5)
        {
            if (buf->seq_status[i] == ESPNOW_DEVICE_ID)
            {
                buf->seq_status[i + 1] = 253;
                buf->seq_status[i + 2] = 254;
                buf->seq_status[i + 3] = ESPNOW_SWITCH;
                if (send_param->seq_cmd[0] == ESPNOW_DEVICE_ID || rx_cmd[0] == ESPNOW_DEVICE_ID)
                {
                    if (rx_cmd[0] == ESPNOW_DEVICE_ID)
                    {
                        buf->seq_status[i + 4] = rx_cmd[1];
                        for (int j = 0; j < 2; j++)
                        {
                            rx_cmd[j] = 0;
                            buf->seq_cmd[j] = 0;
                        }
                    }
                    else if (send_param->seq_cmd[0] == ESPNOW_DEVICE_ID)
                    {
                        buf->seq_status[i + 4] = send_param->seq_cmd[1];
                        for (int j = 0; j < 2; j++)
                        {
                            send_param->seq_cmd[j] = 0;
                            buf->seq_cmd[j] = 0;
                        }
                    }
                }
                else
                { 
                    buf->seq_status[i + 4] = store_status[i + 4];
                    if (rx_cmd[0] != 0)
                    {
                        send_param->seq_cmd[0] = rx_cmd[0];
                        send_param->seq_cmd[1] = rx_cmd[1];
                    }

                    buf->seq_cmd[0] = send_param->seq_cmd[0];
                    buf->seq_cmd[1] = send_param->seq_cmd[1];
                }

                break;
            }
            else
            {
                if (rx_cmd[0] == buf->seq_status[i]) //if (rx_cmd[0] == i)
                {
                    if (rx_cmd[1] == buf->seq_status[i + 4])
                    {
                        for (int j = 0; j < 2; j++)
                        {
                            rx_cmd[j] = 0;
                            buf->seq_cmd[j] = 0;
                        }
                    }
                }
            }
        }
    }

    for (int i = 0; i < n_status; i++)
    {
        ESP_LOGI(TAG, "%d, %d",store_status[i],buf->seq_status[i]);
    }

    save_status = false;
    if(is_internet_connected){
        bool is_diff=true;
        // uint8_t *send_units={0};
        bool is_diff_key=true;
        uint8_t *send_keys={0};
        
        buf->seq_status[5]=5;
        buf->seq_status[10]=15;
        buf->seq_status[15]=33;
        buf->seq_status[20]=14;

        for (int i = 2,j=-1,k=5,x=-1; i < n_status; i=i+5,k=k+5)
        {
            // if(store_status[i] != buf->seq_status[i]){
            //     if(is_diff){
            //         is_diff=false;
            //         send_units =(uint8_t *) calloc(8, sizeof(uint8_t));   
            //     }     
            //     send_units[++j]=buf->seq_status[i-1];
            //     send_units[++j]=buf->seq_status[i];
            // }
            
            if(store_status[k] != buf->seq_status[k]){
                if(is_diff_key){
                    is_diff_key=false;
                    send_keys =(uint8_t *) calloc(40, sizeof(uint8_t));   
                }   

                send_keys[++x]=buf->seq_status[k-4];
                for(int y=1; y<5; y++){

                    ESP_LOGI(TAG, "get store_status Bit %d",getBit(store_status[k],y-1));
                    ESP_LOGI(TAG, "get buf status: %d",getBit(buf->seq_status[k],y-1));                    
                    
                       if(getBit(store_status[k],y-1)!=getBit(buf->seq_status[k],y-1)){
                        send_keys[++x]=y;
                        send_keys[++x]=getBit(buf->seq_status[k],y-1);
                       }else{
                        send_keys[++x]=0;
                        send_keys[++x]=0;
                       } 
                }                
            }
        }
        // if(is_diff ==false || is_diff_key==false){
        //         save_status = true;
        // }
           
        for (int i = 0; i < 20; i++)
        {
                ESP_LOGI(TAG, "send_keys %d",send_keys[i]);
        }   
        // if(!is_diff){
        //     //POST Call request param -> device_id,units
        //     xTaskCreate(http_store_unit_task, "http_store_unit_task", 8192, send_units, 5, NULL);
        // }
        if(!is_diff_key){
            //POST Call request param -> swtich_id,status
            xTaskCreate(http_store_keys_task, "http_store_keys_task", 8192, send_keys, 5, NULL);
        }
        
    }else{
            for (int i = 0; i < n_status; i++)
            {
                if (store_status[i] != buf->seq_status[i])
                {
                   // save_status = true;
                    break;
                }
            }
     }
    
    if (save_status)
    {
        for (int i = 0; i < n_status; i++)
        {
            store_status[i] = buf->seq_status[i];
        }
    }
    ESP_LOGI(TAG, "ESPNOW Sending...");
    
    // for (int i = 0; i < n_status; i++)
    // {
    //     // send_param->seq_status[i] = buf->seq_status[i];
    //     ESP_LOGI(TAG, "Sending Buffer Status: %d", buf->seq_status[i]);
    // }
    // for (int i = 0; i < 2; i++)
    // {
    //     ESP_LOGI(TAG, "Sending Buffer Command: %d", buf->seq_cmd[i]);
    // }

    //esp_fill_random(buf->payload, send_param->len - sizeof(example_espnow_data_t));
    buf->crc = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, send_param->len);
}

static void example_espnow_task(void *pvParameter)
{
    example_espnow_event_t evt;

    vTaskDelay(5000 / portTICK_RATE_MS);

    /* Start sending broadcast ESPNOW data. */
    example_espnow_send_param_t *send_param = (example_espnow_send_param_t *)pvParameter;
    int ret = esp_now_send(send_param->broadcast_mac, send_param->buffer, send_param->len);
    ESP_LOGI(TAG, "Start sending broadcast data: %d",ret);
    if (ret != ESP_OK)
    {

        if (ret == ESP_ERR_ESPNOW_NOT_INIT)
        {
            ESP_LOGE(TAG, "ESP_ERR_ESPNOW_NOT_INIT: %d", ret);
        }
        else if (ret == ESP_ERR_ESPNOW_ARG)
        {
            ESP_LOGE(TAG, "ESP_ERR_ESPNOW_ARG: %d", ret);
        }
        else if (ret == ESP_ERR_ESPNOW_NO_MEM)
        {
            ESP_LOGE(TAG, "ESP_ERR_ESPNOW_NO_MEM: %d", ret);
        }
        else if (ret == ESP_ERR_ESPNOW_FULL)
        {
            ESP_LOGE(TAG, "ESP_ERR_ESPNOW_FULL: %d", ret);
        }
        else if (ret == ESP_ERR_ESPNOW_NOT_FOUND)
        {
            ESP_LOGE(TAG, "ESP_ERR_ESPNOW_NOT_FOUND: %d", ret);
        }
        else if (ret == ESP_ERR_ESPNOW_INTERNAL)
        {
            ESP_LOGE(TAG, "ESP_ERR_ESPNOW_INTERNAL: %d", ret);
        }
        else if (ret == ESP_ERR_ESPNOW_EXIST)
        {
            ESP_LOGE(TAG, "ESP_ERR_ESPNOW_EXIST: %d", ret);
        }
        else if (ret == ESP_ERR_ESPNOW_IF)
        {
            ESP_LOGE(TAG, "ESP_ERR_ESPNOW_IF: %d", ret);
        }

        ESP_LOGE(TAG, "Send error: %d", ret);
        example_espnow_deinit(send_param);
        vTaskDelete(NULL);
    }
    
   
    while (xQueueReceive(s_example_espnow_queue, &evt, portMAX_DELAY) == pdTRUE)
    {
        switch (evt.id)
        {
        case EXAMPLE_ESPNOW_SEND_CB:
        {
            example_espnow_event_send_cb_t *send_cb = &evt.info.send_cb;

            /* Delay a while before sending the next data. */
            if (send_param->delay > 0)
            {
                vTaskDelay(send_param->delay / portTICK_RATE_MS);
            }

            //ESP_LOGI(TAG, "send data to " MACSTR "", MAC2STR(send_cb->mac_addr));

            memcpy(send_param->broadcast_mac, send_cb->mac_addr, ESP_NOW_ETH_ALEN);
            example_espnow_data_prepare(send_param);
            

            /* Send the next data after the previous data is sent. */
            if (esp_now_send(send_param->broadcast_mac, send_param->buffer, send_param->len) != ESP_OK)
            {
                ESP_LOGE(TAG, "Send error");
                example_espnow_deinit(send_param);
                vTaskDelete(NULL);
            }

            break;
        }
        case EXAMPLE_ESPNOW_RECV_CB:
        {

            example_espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;

            example_espnow_data_parse(send_param, recv_cb->mac_addr, recv_cb->data, recv_cb->data_len);
            free(recv_cb->data);

           // ESP_LOGI(TAG, "Receive data from: " MACSTR ", len: %d", MAC2STR(recv_cb->mac_addr), recv_cb->data_len);

            break;
        }
        default:
            ESP_LOGE(TAG, "Callback type error: %d", evt.id);
            break;
        }
    }
}

static esp_err_t example_espnow_init(bool mode,int channel)
{
    example_espnow_send_param_t *send_param;
    
   
    s_example_espnow_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(example_espnow_event_t));
    if (s_example_espnow_queue == NULL)
    {
        ESP_LOGE(TAG, "Create mutex fail");
        return ESP_FAIL;
    }

    /* Initialize ESPNOW and register sending and receiving callback function. */
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(example_espnow_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(example_espnow_recv_cb));

    /* Set primary master key. */
    ESP_ERROR_CHECK(esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK));
    

    /* Add broadcast peer information to peer list. */
    esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
    if (peer == NULL)
    {
        ESP_LOGE(TAG, "Malloc peer information fail");
        vSemaphoreDelete(s_example_espnow_queue);
        esp_now_deinit();
        return ESP_FAIL;
    }
    memset(peer, 0, sizeof(esp_now_peer_info_t));
    peer->channel = channel;
    peer->ifidx = mode;
    peer->encrypt = false;
    memcpy(peer->peer_addr, s_example_broadcast_mac, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK(esp_now_add_peer(peer));
    free(peer);

    /* Initialize sending parameters. */
    send_param = malloc(sizeof(example_espnow_send_param_t));
    memset(send_param, 0, sizeof(example_espnow_send_param_t));
    if (send_param == NULL)
    {
        ESP_LOGE(TAG, "Malloc send parameter fail");
        vSemaphoreDelete(s_example_espnow_queue);
        esp_now_deinit();
        return ESP_FAIL;
    }

    send_param->delay = CONFIG_ESPNOW_SEND_DELAY;
    send_param->len = CONFIG_ESPNOW_SEND_LEN;
    send_param->buffer = malloc(CONFIG_ESPNOW_SEND_LEN);

    if (send_param->buffer == NULL)
    {
        ESP_LOGE(TAG, "Malloc send buffer fail");
        free(send_param);
        vSemaphoreDelete(s_example_espnow_queue);
        esp_now_deinit();
        return ESP_FAIL;
    }
    memcpy(send_param->broadcast_mac, s_example_broadcast_mac, ESP_NOW_ETH_ALEN);
    n_status = (int)(sizeof(send_param->seq_status) / sizeof(send_param->seq_status[0]));
    for (int i = 0; i < n_status; i++)
    {
        send_param->seq_status[i] = store_status[i];
    }

    send_param->seq_cmd[0] = rx_cmd[0];
    send_param->seq_cmd[1] = rx_cmd[1];

    example_espnow_data_prepare(send_param);

    xTaskCreate(example_espnow_task, "example_espnow_task", 2048, send_param, 4, NULL);
    xTaskCreate(do_operation, "do_operation", 4096, NULL, 4, NULL);

    return ESP_OK;
}

static void example_espnow_deinit(example_espnow_send_param_t *send_param)
{
    free(send_param->buffer);
    free(send_param);
    vSemaphoreDelete(s_example_espnow_queue);
    esp_now_deinit();
}

static void init_station_mqtt_server(void *pvParameters)
{
    while (1)
    {
        vTaskDelay(3000 / portTICK_RATE_MS);
        if (rx_cmd[0] == 255)
        {
            rx_cmd[0] = 0;
            
            esp_now_deinit();
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            esp_wifi_disconnect();
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            esp_wifi_stop();
            esp_wifi_deinit();
            vTaskDelay(1000 / portTICK_PERIOD_MS);

            ESP_ERROR_CHECK(esp_netif_init());
            s_wifi_event_group = xEventGroupCreate();
            esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
            assert(sta_netif);
            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            ESP_ERROR_CHECK(esp_wifi_init(&cfg));
            
            

            wifi_config_t wifi_config = {
            .ap = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID),
            .channel = EXAMPLE_ESP_WIFI_CHANNEL, 
            .password = EXAMPLE_ESP_WIFI_PASS,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK},

            .sta = {
            .ssid = "AnandRaman",
            .password = "kunjan@123",
            
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK,

            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

            esp_event_handler_instance_t instance_any_id;
            esp_event_handler_instance_t instance_got_ip;
            ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,ESP_EVENT_ANY_ID,&event_handler,NULL,&instance_any_id));
            ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,IP_EVENT_STA_GOT_IP,&event_handler,NULL,&instance_got_ip));
           // ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
            ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
            ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
            ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
            ESP_ERROR_CHECK(esp_wifi_start());

            vTaskDelay(15000 / portTICK_PERIOD_MS);
            example_espnow_init(0,EXAMPLE_ESP_WIFI_CHANNEL);
            //mqtt_app_start();
        }
    }
    vTaskDelete(NULL);
}

void app_main(void)
{

    //Get MAC address for WiFi Station interface
    ESP_ERROR_CHECK(esp_read_mac(device_mac_addr, ESP_MAC_WIFI_STA));
    ESP_LOGI("WIFI_STA MAC", "0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x", device_mac_addr[0], device_mac_addr[1], device_mac_addr[2], device_mac_addr[3], device_mac_addr[4], device_mac_addr[5]);

    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("MQTT_EXAMPLE", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    SPIFFS_Init();
    gpio_init();

    init_softap_tcp_server();

    store_status = (uint8_t *)malloc(sizeof(uint8_t) * n_status);
    memset(store_status, 0, sizeof(uint8_t) * n_status);

    store_status = getStatusFromMemory();

    xTaskCreate(init_station_mqtt_server, "init_station_mqtt_server", 4096, (void *)AF_INET, 5, NULL);
    
    example_espnow_init(1,EXAMPLE_ESP_WIFI_CHANNEL);
}