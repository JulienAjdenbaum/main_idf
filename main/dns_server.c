#include "dns_server.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "DNS_SERVER";

#define HIJACK_IP_1 192
#define HIJACK_IP_2 168
#define HIJACK_IP_3   4
#define HIJACK_IP_4   1

#pragma pack(push,1)
struct dns_header {
    uint16_t id;
    uint8_t flags1;
    uint8_t flags2;
    uint16_t qcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t adcount;
};
#pragma pack(pop)

static void dns_write_u16(uint8_t *buf, int pos, uint16_t val)
{
    buf[pos]   = val >> 8;
    buf[pos+1] = val & 0xff;
}

static void dns_server_task(void *pv)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "Socket creation failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in server_addr = {0};
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(53);

    if (bind(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed on DNS port 53");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS server started on port 53, hijacking to 192.168.4.1");

    while (true) {
        uint8_t rx_buffer[512];
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);

        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0,
                           (struct sockaddr *)&source_addr, &socklen);
        if (len <= 0) {
            // Possibly error or no data
            continue;
        }

        if (len < sizeof(struct dns_header)) {
            // too small for DNS
            continue;
        }

        struct dns_header *hdr = (struct dns_header *)rx_buffer;
        uint16_t qcount = ntohs(hdr->qcount);
        if (qcount == 0) {
            continue;
        }

        // Turn on response bit + recursion available
        hdr->flags1 = 0x84;  // 1000 0100
        hdr->flags2 = 0x00;
        hdr->ancount = htons(1); // 1 answer

        int offset = sizeof(struct dns_header);

        // skip question domain name
        while (offset < len && rx_buffer[offset] != 0) {
            offset += (rx_buffer[offset] + 1);
        }
        offset += 1; // skip final 0
        // skip QTYPE + QCLASS
        offset += 4;

        // write our answer
        rx_buffer[offset++] = 0xc0;
        rx_buffer[offset++] = 0x0c;  // pointer to the domain name in the question
        dns_write_u16(rx_buffer, offset, 0x0001); // TYPE A
        offset += 2;
        dns_write_u16(rx_buffer, offset, 0x0001); // CLASS IN
        offset += 2;

        // TTL
        rx_buffer[offset++] = 0x00;
        rx_buffer[offset++] = 0x00;
        rx_buffer[offset++] = 0x00;
        rx_buffer[offset++] = 60; // 60s

        // RDLENGTH = 4
        dns_write_u16(rx_buffer, offset, 4);
        offset += 2;

        // RDATA => 192.168.4.1
        rx_buffer[offset++] = HIJACK_IP_1;
        rx_buffer[offset++] = HIJACK_IP_2;
        rx_buffer[offset++] = HIJACK_IP_3;
        rx_buffer[offset++] = HIJACK_IP_4;

        sendto(sock, rx_buffer, offset, 0,
               (struct sockaddr *)&source_addr, sizeof(source_addr));
    }

    close(sock);
    vTaskDelete(NULL);
}

void dns_server_start(void)
{
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, NULL);
}
