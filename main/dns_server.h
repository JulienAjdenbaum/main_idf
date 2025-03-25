#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Starts a minimal DNS server that hijacks all DNS queries.
 *
 * Listens on UDP port 53. For any hostname requested,
 * it always returns 192.168.4.1 (AP IP).
 */
void dns_server_start(void);

#ifdef __cplusplus
}
#endif
