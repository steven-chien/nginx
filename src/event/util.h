#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>
#include <netinet/in.h>

void print_mac_address(unsigned char *mac);
int get_mac_address(const char *ifname, struct sockaddr_in addr, uint8_t *mac);
void hexdump(const char *title, void *buf, size_t len);
double elapsed_time(struct timespec a, struct timespec b);
void get_datetime_str(char *buf, size_t length);
void convertToISODateTime(const char* inputDateTime, char* outputISODateTime);
void unescapeHtml(char* str);

#endif
