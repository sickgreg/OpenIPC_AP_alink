#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <getopt.h>

#define MIN_BITRATE 500     // kbps
#define MAX_BITRATE 25000   // kbps

#define RSSI_MIN 0
#define RSSI_MAX 1

// Default PID parameters (tuned for more aggressive response)
float Kp = 1.0f;
float Ki = 0.05f;
float Kd = 0.4f;

// Cooldown timing (ms)
#define STRICT_COOLDOWN 200
#define UP_COOLDOWN     3000
#define MIN_CHANGE_DELTA_PCT 5

int last_bitrate = 0;
int last_error = 0;
float integral = 0.0f;
unsigned long last_change_time = 0;
unsigned long last_up_time = 0;

// Time in ms
unsigned long current_millis() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

// Parse STA info
int parse_sta_info(int *rssi_out, int *bitrate_out, const char *current_mac) {
    FILE *fp = fopen("/proc/net/rtl88x2eu/wlan0/all_sta_info", "r");
    if (!fp) return 0;

    char line[256];
    int mac_match = 0;
    char mac[32] = {0};

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "sta's macaddr:", 14) == 0) {
            sscanf(line + 14, "%31s", mac);
            mac_match = (strcmp(mac, "ff:ff:ff:ff:ff:ff") != 0 && strcmp(mac, current_mac) != 0);
        } else if (mac_match) {
            if (strstr(line, "vht_bitrate=")) {
                sscanf(strstr(line, "vht_bitrate="), "vht_bitrate=%d", bitrate_out);
            } else if (strstr(line, "tx_bitrate_100kbps=")) {
                int tmp = 0;
                sscanf(strstr(line, "tx_bitrate_100kbps="), "tx_bitrate_100kbps=%d", &tmp);
                *bitrate_out = tmp * 100 / 1000;
            } else if (strstr(line, "rssi=")) {
                sscanf(strstr(line, "rssi="), "rssi=%d", rssi_out);
            }
        }
    }

    fclose(fp);
    return (*bitrate_out > 0);
}

// Replaces system(wget) with raw HTTP GET
void set_bitrate(int bitrate) {
    int s;
    struct sockaddr_in addr;
    char req[128];
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0) return;

    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s);
        return;
    }

    snprintf(req, sizeof(req),
        "GET /api/v1/set?video0.bitrate=%d HTTP/1.0\r\n\r\n", bitrate);
    if (send(s, req, strlen(req), 0) < 0) {
        close(s);
        return;
    }

    char buf[64];
    while (recv(s, buf, sizeof(buf), 0) > 0);

    close(s);
}

void parse_args(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "p:i:d:")) != -1) {
        switch (opt) {
            case 'p': Kp = atof(optarg); break;
            case 'i': Ki = atof(optarg); break;
            case 'd': Kd = atof(optarg); break;
            default:
                fprintf(stderr, "Usage: %s [-p Kp] [-i Ki] [-d Kd]\n", argv[0]);
                exit(1);
        }
    }
}

int main(int argc, char **argv) {
    parse_args(argc, argv);

    char current_mac[32] = {0};
    FILE *mac_cmd = popen("ip link show wlan0 | awk '/link\\/ether/ {print $2}'", "r");
    if (mac_cmd && fgets(current_mac, sizeof(current_mac), mac_cmd)) {
        strtok(current_mac, "\n");
        pclose(mac_cmd);
    } else {
        fprintf(stderr, "Failed to get MAC address\n");
        return 1;
    }

    printf("PID: Kp=%.2f, Ki=%.2f, Kd=%.2f\n", Kp, Ki, Kd);

    while (1) {
        int rssi = -100;
        int raw_bitrate = 0;
        int valid = parse_sta_info(&rssi, &raw_bitrate, current_mac);
        if (!valid) {
            usleep(100000);
            continue;
        }

        int raw_kbps = raw_bitrate * 1000;
        int encoder_limit = raw_kbps * 25 / 100;
        if (encoder_limit < MIN_BITRATE) encoder_limit = MIN_BITRATE;
        if (encoder_limit > MAX_BITRATE) encoder_limit = MAX_BITRATE;

        int rssi_factor = MAX_BITRATE;
        if (rssi < RSSI_MIN) rssi_factor = MIN_BITRATE;
        else if (rssi <= RSSI_MAX) {
            int range = RSSI_MAX - RSSI_MIN;
            int rel = rssi - RSSI_MIN;
            rssi_factor = MIN_BITRATE + rel * (MAX_BITRATE - MIN_BITRATE) / (range ? range : 1);
        }

        int target = (encoder_limit < rssi_factor) ? encoder_limit : rssi_factor;

        // Emergency drop
        unsigned long now = current_millis();
        if (encoder_limit < last_bitrate * 70 / 100) {
            set_bitrate(encoder_limit);
            printf("EMERGENCY DROP to %d kbps (link: %d kbps, RSSI: %d)\n", encoder_limit, encoder_limit, rssi);
            last_bitrate = encoder_limit;
            last_change_time = now;
            last_up_time = now;
            integral = 0;
            last_error = 0;
            usleep(100000);
            continue;
        }

        // PID logic
        int error = target - last_bitrate;
        integral += error;
        int derivative = error - last_error;
        int adjustment = (int)(Kp * error + Ki * integral + Kd * derivative);
        int final_bitrate = last_bitrate + adjustment;

        if (final_bitrate < MIN_BITRATE) final_bitrate = MIN_BITRATE;
        if (final_bitrate > MAX_BITRATE) final_bitrate = MAX_BITRATE;

        // Cooldown and threshold logic
        int delta = abs(final_bitrate - last_bitrate);
        int min_delta = last_bitrate * MIN_CHANGE_DELTA_PCT / 100;
        int should_change = 0;

        if (delta >= min_delta) {
            if (final_bitrate < last_bitrate && (now - last_change_time) >= STRICT_COOLDOWN) {
                should_change = 1;
            } else if (final_bitrate > last_bitrate &&
                       (now - last_change_time) >= STRICT_COOLDOWN &&
                       (now - last_up_time) >= UP_COOLDOWN) {
                should_change = 1;
            }
        }

        if (should_change) {
            set_bitrate(final_bitrate);
            printf("Bitrate changed to %d kbps (link: %d kbps, RSSI: %d)\n",
                   final_bitrate, encoder_limit, rssi);
            if (final_bitrate > last_bitrate)
                last_up_time = now;
            last_change_time = now;
            last_bitrate = final_bitrate;
        }

        last_error = error;
        usleep(100000);
    }

    return 0;
}
