#if defined(__APPLE__) || defined(__FreeBSD__)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <net/if_dl.h>
#elif defined(__linux__)
#include <netpacket/packet.h>
#endif
#else
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <process.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Iphlpapi.lib")
#pragma comment(lib, "Advapi32.lib")
#endif

#define SOL_VERSION "0.1.0"
#define SOL_SERVICE_NAME "SleepOnLan"
#define SOL_SERVICE_DISPLAY_NAME "Sleep on LAN"
#define SOL_SERVICE_DESCRIPTION "Listens for inverse Wake-on-LAN packets and puts this computer to sleep."
#define MAX_LISTENERS 16
#define MAX_COMMANDS 32
#define MAX_MACS 64
#define MAX_STR 256
#define HTTP_BUF 4096
#define MAGIC_PACKET_SIZE 102

typedef struct {
    char type[8];
    int port;
} Listener;

typedef struct {
    char operation[64];
    char type[32];
    bool is_default;
    char command[MAX_STR];
} Command;

typedef struct {
    bool active;
    long delay_ms;
} DelayConfig;

typedef struct {
    Listener listeners[MAX_LISTENERS];
    int listener_count;
    char log_level[16];
    char broadcast_ip[64];
    char http_output[16];
    bool exit_if_port_used;
    char auth_login[128];
    char auth_password[128];
    DelayConfig avoid_dual_udp;
    DelayConfig delay_before_commands;
    Command commands[MAX_COMMANDS];
    int command_count;
    bool verbose;
} Config;

typedef struct {
    uint8_t bytes[6];
    char text[18];
    char reversed[18];
    char name[128];
} LocalMac;

typedef struct {
    Config *config;
    int port;
} ListenerArgs;

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_udp_action_pending = 0;
static LocalMac g_local_macs[MAX_MACS];
static int g_local_mac_count = 0;

#ifdef _WIN32
static SERVICE_STATUS_HANDLE g_service_status_handle = NULL;
static SERVICE_STATUS g_service_status;
static bool g_running_as_service = false;
static const char *g_service_config_arg = NULL;

typedef uintptr_t thread_t;
typedef SOCKET socket_t;
typedef int socket_len_t;
typedef unsigned (__stdcall *thread_fn_t)(void *);
#define INVALID_SOCKET_T INVALID_SOCKET
#define THREAD_RET unsigned __stdcall
#define THREAD_RETURN 0
#define close_socket closesocket
#else
typedef pthread_t thread_t;
typedef int socket_t;
typedef socklen_t socket_len_t;
typedef void *(*thread_fn_t)(void *);
#define INVALID_SOCKET_T (-1)
#define THREAD_RET void *
#define THREAD_RETURN NULL
#define close_socket close
#endif

#ifdef _WIN32
static void windows_event_log(WORD type, const char *message) {
    HANDLE event_source = RegisterEventSourceA(NULL, SOL_SERVICE_NAME);
    if (!event_source) {
        return;
    }
    LPCSTR strings[1];
    strings[0] = message;
    ReportEventA(event_source, type, 0, 0, NULL, 1, 0, strings, NULL);
    DeregisterEventSource(event_source);
}
#endif

static void log_msg(const char *level, const char *fmt, ...) {
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    struct tm *tmp = localtime(&now);
    if (tmp) {
        tmv = *tmp;
    } else {
        memset(&tmv, 0, sizeof(tmv));
    }
#else
    localtime_r(&now, &tmv);
#endif
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);
    char message[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s [%s] %s\n", stamp, level, message);
#ifdef _WIN32
    if (g_running_as_service) {
        WORD type = EVENTLOG_INFORMATION_TYPE;
        if (strcmp(level, "ERROR") == 0) {
            type = EVENTLOG_ERROR_TYPE;
        } else if (strcmp(level, "WARN") == 0) {
            type = EVENTLOG_WARNING_TYPE;
        }
        windows_event_log(type, message);
    }
#endif
}

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static const char *socket_error_message(void) {
#ifdef _WIN32
    static char buf[128];
    snprintf(buf, sizeof(buf), "WSA error %d", WSAGetLastError());
    return buf;
#else
    return strerror(errno);
#endif
}

static void sleep_ms(long ms) {
    if (ms <= 0) {
        return;
    }
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
    }
#endif
}

static long parse_duration_ms(const char *s, long fallback) {
    if (!s || !*s) {
        return fallback;
    }
    char *end = NULL;
    double value = strtod(s, &end);
    if (end == s || value < 0) {
        return fallback;
    }
    while (*end && isspace((unsigned char)*end)) {
        end++;
    }
    if (strcmp(end, "ms") == 0 || *end == '\0') {
        return (long)value;
    }
    if (strcmp(end, "s") == 0) {
        return (long)(value * 1000.0);
    }
    if (strcmp(end, "m") == 0) {
        return (long)(value * 60000.0);
    }
    return fallback;
}

static void mac_to_string(const uint8_t mac[6], char out[18]) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void reverse_mac_bytes(const uint8_t in[6], uint8_t out[6]) {
    for (int i = 0; i < 6; i++) {
        out[i] = in[5 - i];
    }
}

static void reverse_mac_string(const char *mac, char out[18]) {
    unsigned int b[6];
    if (sscanf(mac, "%2x:%2x:%2x:%2x:%2x:%2x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        out[0] = '\0';
        return;
    }
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             b[5], b[4], b[3], b[2], b[1], b[0]);
}

static bool parse_mac(const char *text, uint8_t mac[6]) {
    unsigned int b[6];
    if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (b[i] > 255) {
            return false;
        }
        mac[i] = (uint8_t)b[i];
    }
    return true;
}

static bool all_zero_mac(const uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0) {
            return false;
        }
    }
    return true;
}

static void add_local_mac(const char *name, const uint8_t mac[6]) {
    if (g_local_mac_count >= MAX_MACS || all_zero_mac(mac)) {
        return;
    }
    for (int i = 0; i < g_local_mac_count; i++) {
        if (memcmp(g_local_macs[i].bytes, mac, 6) == 0) {
            return;
        }
    }
    LocalMac *entry = &g_local_macs[g_local_mac_count++];
    memcpy(entry->bytes, mac, 6);
    mac_to_string(mac, entry->text);
    reverse_mac_string(entry->text, entry->reversed);
    snprintf(entry->name, sizeof(entry->name), "%s", name ? name : "interface");
}

static void load_local_macs(void) {
    g_local_mac_count = 0;
#ifdef _WIN32
    ULONG size = 0;
    GetAdaptersAddresses(AF_UNSPEC, 0, NULL, NULL, &size);
    IP_ADAPTER_ADDRESSES *adapters = (IP_ADAPTER_ADDRESSES *)malloc(size);
    if (!adapters) {
        return;
    }
    if (GetAdaptersAddresses(AF_UNSPEC, 0, NULL, adapters, &size) == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES *a = adapters; a; a = a->Next) {
            if (a->PhysicalAddressLength == 6) {
                add_local_mac(a->AdapterName, a->PhysicalAddress);
            }
        }
    }
    free(adapters);
#else
    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) == -1) {
        log_msg("ERROR", "getifaddrs failed: %s", strerror(errno));
        return;
    }
    for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || (ifa->ifa_flags & IFF_LOOPBACK)) {
            continue;
        }
#if defined(__APPLE__) || defined(__FreeBSD__)
        if (ifa->ifa_addr->sa_family == AF_LINK) {
            struct sockaddr_dl *sdl = (struct sockaddr_dl *)ifa->ifa_addr;
            if (sdl->sdl_alen == 6) {
                add_local_mac(ifa->ifa_name, (const uint8_t *)LLADDR(sdl));
            }
        }
#elif defined(__linux__)
        if (ifa->ifa_addr->sa_family == AF_PACKET) {
            struct sockaddr_ll *sll = (struct sockaddr_ll *)ifa->ifa_addr;
            if (sll->sll_halen == 6) {
                add_local_mac(ifa->ifa_name, (const uint8_t *)sll->sll_addr);
            }
        }
#endif
    }
    freeifaddrs(ifaddr);
#endif
}

static const char *skip_ws(const char *p) {
    while (p && *p && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

static const char *find_key(const char *json, const char *key) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    return strstr(json, needle);
}

static bool json_get_string(const char *json, const char *key, char *out, size_t out_size) {
    const char *p = find_key(json, key);
    if (!p) {
        return false;
    }
    p = strchr(p, ':');
    if (!p) {
        return false;
    }
    p = skip_ws(p + 1);
    if (*p != '"') {
        return false;
    }
    p++;
    size_t n = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
        }
        if (n + 1 < out_size) {
            out[n++] = *p;
        }
        p++;
    }
    out[n] = '\0';
    return true;
}

static bool json_get_bool(const char *json, const char *key, bool *out) {
    const char *p = find_key(json, key);
    if (!p) {
        return false;
    }
    p = strchr(p, ':');
    if (!p) {
        return false;
    }
    p = skip_ws(p + 1);
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static const char *json_section(const char *json, const char *key, char open_ch, char close_ch, size_t *len) {
    const char *p = key && *key ? find_key(json, key) : json;
    if (!p) {
        return NULL;
    }
    if (key && *key) {
        p = strchr(p, ':');
        if (!p) {
            return NULL;
        }
    }
    p = strchr(p, open_ch);
    if (!p) {
        return NULL;
    }
    int depth = 0;
    bool in_string = false;
    bool escape = false;
    for (const char *q = p; *q; q++) {
        if (escape) {
            escape = false;
            continue;
        }
        if (in_string && *q == '\\') {
            escape = true;
            continue;
        }
        if (*q == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) {
            continue;
        }
        if (*q == open_ch) {
            depth++;
        } else if (*q == close_ch) {
            depth--;
            if (depth == 0) {
                *len = (size_t)(q - p + 1);
                return p;
            }
        }
    }
    return NULL;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *)calloc((size_t)size + 1, 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size && ferror(f)) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return buf;
}

static void add_listener(Config *cfg, const char *spec) {
    if (cfg->listener_count >= MAX_LISTENERS || !spec || !*spec) {
        return;
    }
    Listener *l = &cfg->listeners[cfg->listener_count];
    char temp[64];
    snprintf(temp, sizeof(temp), "%s", spec);
    char *colon = strchr(temp, ':');
    int port = 0;
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
    }
    for (char *p = temp; *p; p++) {
        *p = (char)toupper((unsigned char)*p);
    }
    if (strcmp(temp, "UDP") == 0) {
        snprintf(l->type, sizeof(l->type), "UDP");
        l->port = port > 0 ? port : 9;
    } else if (strcmp(temp, "HTTP") == 0) {
        snprintf(l->type, sizeof(l->type), "HTTP");
        l->port = port > 0 ? port : 8009;
    } else {
        log_msg("WARN", "Ignoring unknown listener '%s'", spec);
        return;
    }
    cfg->listener_count++;
}

static void add_command(Config *cfg, const char *operation, const char *type, bool is_default, const char *command) {
    if (cfg->command_count >= MAX_COMMANDS || !operation || !*operation) {
        return;
    }
    Command *cmd = &cfg->commands[cfg->command_count++];
    snprintf(cmd->operation, sizeof(cmd->operation), "%s", operation);
    snprintf(cmd->type, sizeof(cmd->type), "%s", (type && *type) ? type : "external");
    snprintf(cmd->command, sizeof(cmd->command), "%s", command ? command : "");
    cmd->is_default = is_default;
}

static void init_default_config(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->log_level, sizeof(cfg->log_level), "INFO");
    snprintf(cfg->broadcast_ip, sizeof(cfg->broadcast_ip), "192.168.255.255");
    snprintf(cfg->http_output, sizeof(cfg->http_output), "XML");
    cfg->avoid_dual_udp.active = false;
    cfg->avoid_dual_udp.delay_ms = 100;
    cfg->delay_before_commands.active = true;
    cfg->delay_before_commands.delay_ms = 500;
    add_listener(cfg, "UDP:7");
    add_listener(cfg, "UDP:9");
    add_listener(cfg, "HTTP:8009");
#ifdef _WIN32
    add_command(cfg, "sleep", "external", true, "rundll32.exe powrprof.dll,SetSuspendState 0,1,0");
#elif defined(__APPLE__)
    add_command(cfg, "sleep", "external", true, "pmset sleepnow");
#else
    add_command(cfg, "sleep", "external", true, "systemctl suspend");
#endif
}

static void parse_string_array_listeners(Config *cfg, const char *json) {
    size_t len = 0;
    const char *arr = json_section(json, "Listeners", '[', ']', &len);
    if (!arr) {
        return;
    }
    cfg->listener_count = 0;
    const char *p = arr;
    const char *end = arr + len;
    while (p < end) {
        p = strchr(p, '"');
        if (!p || p >= end) {
            break;
        }
        p++;
        char value[64] = {0};
        size_t n = 0;
        while (p < end && *p && *p != '"') {
            if (n + 1 < sizeof(value)) {
                value[n++] = *p;
            }
            p++;
        }
        add_listener(cfg, value);
        if (p < end) {
            p++;
        }
    }
    if (cfg->listener_count == 0) {
        add_listener(cfg, "UDP:7");
        add_listener(cfg, "UDP:9");
        add_listener(cfg, "HTTP:8009");
    }
}

static void parse_delay_section(const char *json, const char *key, DelayConfig *delay) {
    size_t len = 0;
    const char *section = json_section(json, key, '{', '}', &len);
    if (!section) {
        return;
    }
    char *copy = (char *)calloc(len + 1, 1);
    if (!copy) {
        return;
    }
    memcpy(copy, section, len);
    bool active;
    if (json_get_bool(copy, "Active", &active)) {
        delay->active = active;
    }
    char duration[32];
    if (json_get_string(copy, "Delay", duration, sizeof(duration))) {
        delay->delay_ms = parse_duration_ms(duration, delay->delay_ms);
    }
    free(copy);
}

static void parse_auth_section(Config *cfg, const char *json) {
    size_t len = 0;
    const char *section = json_section(json, "Auth", '{', '}', &len);
    if (!section) {
        return;
    }
    char *copy = (char *)calloc(len + 1, 1);
    if (!copy) {
        return;
    }
    memcpy(copy, section, len);
    json_get_string(copy, "Login", cfg->auth_login, sizeof(cfg->auth_login));
    json_get_string(copy, "Password", cfg->auth_password, sizeof(cfg->auth_password));
    free(copy);
}

static void parse_commands(Config *cfg, const char *json) {
    size_t len = 0;
    const char *arr = json_section(json, "Commands", '[', ']', &len);
    if (!arr) {
        return;
    }
    cfg->command_count = 0;
    const char *p = arr;
    const char *end = arr + len;
    while (p < end) {
        size_t obj_len = 0;
        const char *obj = strchr(p, '{');
        if (!obj || obj >= end) {
            break;
        }
        const char *found = json_section(obj, "", '{', '}', &obj_len);
        if (!found || found >= end) {
            break;
        }
        char *copy = (char *)calloc(obj_len + 1, 1);
        if (!copy) {
            break;
        }
        memcpy(copy, found, obj_len);
        char operation[64] = {0};
        char type[32] = "external";
        char command[MAX_STR] = {0};
        bool is_default = false;
        json_get_string(copy, "Operation", operation, sizeof(operation));
        json_get_string(copy, "Type", type, sizeof(type));
        json_get_string(copy, "Command", command, sizeof(command));
        json_get_bool(copy, "Default", &is_default);
        if (operation[0]) {
            add_command(cfg, operation, type, is_default, command);
        }
        free(copy);
        p = found + obj_len;
    }
    if (cfg->command_count == 1) {
        cfg->commands[0].is_default = true;
    }
    bool has_default = false;
    for (int i = 0; i < cfg->command_count; i++) {
        if (cfg->commands[i].is_default) {
            has_default = true;
            break;
        }
    }
    if (!has_default && cfg->command_count > 0) {
        cfg->commands[0].is_default = true;
    }
}

static bool load_config(Config *cfg, const char *path) {
    if (!path || !*path) {
        return true;
    }
    char *json = read_file(path);
    if (!json) {
        return false;
    }
    log_msg("INFO", "Loaded configuration from %s", path);
    parse_string_array_listeners(cfg, json);
    json_get_string(json, "LogLevel", cfg->log_level, sizeof(cfg->log_level));
    json_get_string(json, "BroadcastIP", cfg->broadcast_ip, sizeof(cfg->broadcast_ip));
    json_get_string(json, "HTTPOutput", cfg->http_output, sizeof(cfg->http_output));
    json_get_bool(json, "ExitIfAnyPortIsAlreadyUsed", &cfg->exit_if_port_used);
    parse_auth_section(cfg, json);
    parse_delay_section(json, "AvoidDualUDPSending", &cfg->avoid_dual_udp);
    parse_delay_section(json, "DelayBeforeCommands", &cfg->delay_before_commands);
    parse_commands(cfg, json);
    free(json);
    return true;
}

#ifdef _WIN32
static bool windows_programdata_config_path(char *out, size_t out_size) {
    char program_data[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("ProgramData", program_data, sizeof(program_data));
    if (n == 0 || n >= sizeof(program_data)) {
        snprintf(program_data, sizeof(program_data), "C:\\ProgramData");
    }
    int written = snprintf(out, out_size, "%s\\SleepOnLan\\sol.json", program_data);
    return written > 0 && (size_t)written < out_size;
}
#endif

static const char *find_config_path(const char *requested) {
    static char selected[512];
#ifdef _WIN32
    char programdata_config[512] = {0};
    windows_programdata_config_path(programdata_config, sizeof(programdata_config));
#endif
    const char *candidates[] = {
        requested,
        "sol.json",
#ifdef _WIN32
        programdata_config,
#else
        "/etc/sol.json",
        "/etc/sleep-on-lan.json",
#endif
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        if (!candidates[i] || !*candidates[i]) {
            continue;
        }
        FILE *f = fopen(candidates[i], "rb");
        if (f) {
            fclose(f);
            snprintf(selected, sizeof(selected), "%s", candidates[i]);
            return selected;
        }
    }
    return NULL;
}

static Command *find_command(Config *cfg, const char *operation) {
    for (int i = 0; i < cfg->command_count; i++) {
        if (strcmp(cfg->commands[i].operation, operation) == 0) {
            return &cfg->commands[i];
        }
    }
    return NULL;
}

static Command *default_command(Config *cfg) {
    for (int i = 0; i < cfg->command_count; i++) {
        if (cfg->commands[i].is_default) {
            return &cfg->commands[i];
        }
    }
    return cfg->command_count ? &cfg->commands[0] : NULL;
}

static void execute_command(Command *cmd) {
    if (!cmd || !cmd->command[0]) {
        log_msg("WARN", "No command configured for operation");
        return;
    }
    if (strcmp(cmd->type, "external") != 0) {
        log_msg("WARN", "Command type '%s' is not supported in this C build; running as external", cmd->type);
    }
    log_msg("INFO", "Executing '%s': %s", cmd->operation, cmd->command);
    int rc = system(cmd->command);
    if (rc == -1) {
        log_msg("ERROR", "system() failed: %s", strerror(errno));
    } else {
        log_msg("INFO", "Command '%s' exited with status %d", cmd->operation, rc);
    }
}

typedef struct {
    Command cmd;
    long delay_ms;
} CommandJob;

static THREAD_RET command_thread(void *arg) {
    CommandJob *job = (CommandJob *)arg;
    sleep_ms(job->delay_ms);
    execute_command(&job->cmd);
    free(job);
    return THREAD_RETURN;
}

static void execute_command_async(Command *cmd, long delay_ms) {
    CommandJob *job = (CommandJob *)calloc(1, sizeof(*job));
    if (!job) {
        return;
    }
    job->cmd = *cmd;
    job->delay_ms = delay_ms;
#ifdef _WIN32
    uintptr_t h = _beginthreadex(NULL, 0, command_thread, job, 0, NULL);
    if (h) {
        CloseHandle((HANDLE)h);
    } else {
        free(job);
    }
#else
    pthread_t t;
    if (pthread_create(&t, NULL, command_thread, job) == 0) {
        pthread_detach(t);
    } else {
        free(job);
    }
#endif
}

static bool extract_magic_packet_mac(const uint8_t *buf, size_t len, uint8_t out[6]) {
    if (len != MAGIC_PACKET_SIZE) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (buf[i] != 0xff) {
            return false;
        }
    }
    for (int i = 0; i < 6; i++) {
        out[i] = buf[6 + i];
    }
    for (int block = 1; block < 16; block++) {
        if (memcmp(buf + 6 + block * 6, out, 6) != 0) {
            return false;
        }
    }
    return true;
}

static bool sleep_packet_matches_local_mac(const uint8_t packet_mac[6], char *matched, size_t matched_size) {
    uint8_t local_order_mac[6];
    reverse_mac_bytes(packet_mac, local_order_mac);
    for (int i = 0; i < g_local_mac_count; i++) {
        if (memcmp(local_order_mac, g_local_macs[i].bytes, 6) == 0) {
            snprintf(matched, matched_size, "%s", g_local_macs[i].text);
            return true;
        }
    }
    return false;
}

static socket_t udp_socket_bind(int port) {
    socket_t fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == INVALID_SOCKET_T) {
        return INVALID_SOCKET_T;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close_socket(fd);
        return INVALID_SOCKET_T;
    }
    return fd;
}

static void run_default_action(Config *cfg) {
    Command *cmd = default_command(cfg);
    if (cmd) {
        execute_command(cmd);
    }
    g_udp_action_pending = 0;
}

static THREAD_RET udp_listener_thread(void *arg) {
    ListenerArgs *args = (ListenerArgs *)arg;
    Config *cfg = args->config;
    int port = args->port;
    free(args);
    socket_t fd = udp_socket_bind(port);
    if (fd == (socket_t)-1) {
        log_msg("ERROR", "Unable to bind UDP port %d: %s", port, socket_error_message());
        if (cfg->exit_if_port_used) {
            g_running = 0;
        }
        return THREAD_RETURN;
    }
    log_msg("INFO", "Listening for UDP magic packets on port %d", port);
    while (g_running) {
        uint8_t buf[1024];
        struct sockaddr_in remote;
        socket_len_t remote_len = (socket_len_t)sizeof(remote);
        int n = recvfrom(fd, (char *)buf, (int)sizeof(buf), 0, (struct sockaddr *)&remote, &remote_len);
        if (n <= 0) {
            continue;
        }
        uint8_t packet_mac[6];
        if (!extract_magic_packet_mac(buf, (size_t)n, packet_mac)) {
            continue;
        }
        char extracted[18];
        mac_to_string(packet_mac, extracted);
        char matched[18];
        if (sleep_packet_matches_local_mac(packet_mac, matched, sizeof(matched))) {
            log_msg("INFO", "Received reversed magic packet for local MAC %s", matched);
            if (cfg->avoid_dual_udp.active) {
                if (g_udp_action_pending) {
                    log_msg("INFO", "Ignoring duplicate UDP action during avoidance window");
                    continue;
                }
                g_udp_action_pending = 1;
                sleep_ms(cfg->avoid_dual_udp.delay_ms);
                run_default_action(cfg);
            } else {
                run_default_action(cfg);
            }
        } else {
            log_msg("INFO", "Received reversed magic packet for non-local MAC %s", extracted);
        }
    }
    close_socket(fd);
    return THREAD_RETURN;
}

static bool make_magic_packet(const char *mac_text, uint8_t packet[102]) {
    uint8_t mac[6];
    if (!parse_mac(mac_text, mac)) {
        return false;
    }
    memset(packet, 0xff, 6);
    for (int i = 0; i < 16; i++) {
        memcpy(packet + 6 + i * 6, mac, 6);
    }
    return true;
}

static bool send_wol(Config *cfg, const char *mac_text) {
    uint8_t packet[102];
    if (!make_magic_packet(mac_text, packet)) {
        return false;
    }
    socket_t fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == (socket_t)-1) {
        return false;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char *)&opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9);
    if (inet_pton(AF_INET, cfg->broadcast_ip, &addr.sin_addr) != 1) {
        close_socket(fd);
        return false;
    }
    int sent = sendto(fd, (const char *)packet, (int)sizeof(packet), 0, (struct sockaddr *)&addr, sizeof(addr));
    close_socket(fd);
    return sent == (int)sizeof(packet);
}

static void xml_escape(const char *in, char *out, size_t out_size) {
    size_t n = 0;
    for (; *in && n + 1 < out_size; in++) {
        const char *rep = NULL;
        if (*in == '&') rep = "&amp;";
        else if (*in == '<') rep = "&lt;";
        else if (*in == '>') rep = "&gt;";
        else if (*in == '"') rep = "&quot;";
        if (rep) {
            size_t rlen = strlen(rep);
            if (n + rlen >= out_size) break;
            memcpy(out + n, rep, rlen);
            n += rlen;
        } else {
            out[n++] = *in;
        }
    }
    out[n] = '\0';
}

static bool wants_json(Config *cfg, const char *path) {
    char upper[16];
    snprintf(upper, sizeof(upper), "%s", cfg->http_output);
    for (char *p = upper; *p; p++) {
        *p = (char)toupper((unsigned char)*p);
    }
    return strcmp(upper, "JSON") == 0 || strstr(path, "format=JSON") || strstr(path, "format=json");
}

static void appendf(char *buf, size_t size, const char *fmt, ...) {
    size_t used = strlen(buf);
    if (used >= size) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + used, size - used, fmt, ap);
    va_end(ap);
}

static void render_root(Config *cfg, char *body, size_t size, bool json) {
    if (json) {
        appendf(body, size, "{\"application\":\"sleep-on-lan\",\"version\":\"%s\",\"hosts\":[", SOL_VERSION);
        for (int i = 0; i < g_local_mac_count; i++) {
            appendf(body, size, "%s{\"interface\":\"%s\",\"mac\":\"%s\",\"reversedMac\":\"%s\"}",
                    i ? "," : "", g_local_macs[i].name, g_local_macs[i].text, g_local_macs[i].reversed);
        }
        appendf(body, size, "],\"listeners\":[");
        for (int i = 0; i < cfg->listener_count; i++) {
            appendf(body, size, "%s{\"type\":\"%s\",\"port\":%d,\"active\":true}",
                    i ? "," : "", cfg->listeners[i].type, cfg->listeners[i].port);
        }
        appendf(body, size, "],\"commands\":[");
        for (int i = 0; i < cfg->command_count; i++) {
            appendf(body, size, "%s{\"operation\":\"%s\",\"type\":\"%s\",\"command\":\"%s\",\"default\":%s}",
                    i ? "," : "", cfg->commands[i].operation, cfg->commands[i].type,
                    cfg->commands[i].command, cfg->commands[i].is_default ? "true" : "false");
        }
        appendf(body, size, "]}");
        return;
    }
    appendf(body, size, "<result application=\"sleep-on-lan\" version=\"%s\"><hosts>", SOL_VERSION);
    for (int i = 0; i < g_local_mac_count; i++) {
        appendf(body, size, "<host interface=\"%s\" mac=\"%s\" reversed-mac=\"%s\"/>",
                g_local_macs[i].name, g_local_macs[i].text, g_local_macs[i].reversed);
    }
    appendf(body, size, "</hosts><listeners>");
    for (int i = 0; i < cfg->listener_count; i++) {
        appendf(body, size, "<listener type=\"%s\" port=\"%d\" active=\"true\"/>",
                cfg->listeners[i].type, cfg->listeners[i].port);
    }
    appendf(body, size, "</listeners><commands>");
    for (int i = 0; i < cfg->command_count; i++) {
        char escaped[MAX_STR * 2];
        xml_escape(cfg->commands[i].command, escaped, sizeof(escaped));
        appendf(body, size, "<command operation=\"%s\" type=\"%s\" default=\"%s\">%s</command>",
                cfg->commands[i].operation, cfg->commands[i].type,
                cfg->commands[i].is_default ? "true" : "false", escaped);
    }
    appendf(body, size, "</commands></result>");
}

static void render_operation(char *body, size_t size, const char *operation, bool ok, bool json) {
    if (json) {
        appendf(body, size, "{\"operation\":\"%s\",\"result\":%s}", operation, ok ? "true" : "false");
    } else {
        appendf(body, size, "<operation name=\"%s\" result=\"%s\"/>", operation, ok ? "true" : "false");
    }
}

static int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

static bool base64_decode(const char *in, uint8_t *out, size_t *out_len) {
    int val = 0;
    int valb = -8;
    size_t n = 0;
    for (; *in; in++) {
        unsigned char c = (unsigned char)*in;
        if (isspace(c)) {
            continue;
        }
        if (c == '=') {
            break;
        }
        int digit = base64_value(c);
        if (digit < 0) {
            return false;
        }
        val = (val << 6) + digit;
        valb += 6;
        if (valb >= 0) {
            if (n >= *out_len) {
                return false;
            }
            out[n++] = (uint8_t)((val >> valb) & 0xff);
            valb -= 8;
        }
    }
    *out_len = n;
    return true;
}

static bool check_auth(Config *cfg, const char *request) {
    if (!cfg->auth_login[0] && !cfg->auth_password[0]) {
        return true;
    }
    const char *p = strstr(request, "\nAuthorization:");
    if (!p) {
        p = strstr(request, "\nauthorization:");
    }
    if (!p) {
        return false;
    }
    p = strstr(p, "Basic ");
    if (!p) {
        return false;
    }
    p += 6;
    char encoded[256] = {0};
    size_t n = 0;
    while (*p && *p != '\r' && *p != '\n' && n + 1 < sizeof(encoded)) {
        encoded[n++] = *p++;
    }
    uint8_t decoded[256];
    size_t decoded_len = sizeof(decoded) - 1;
    if (!base64_decode(encoded, decoded, &decoded_len)) {
        return false;
    }
    decoded[decoded_len] = '\0';
    char expected[256];
    snprintf(expected, sizeof(expected), "%s:%s", cfg->auth_login, cfg->auth_password);
    return strcmp((char *)decoded, expected) == 0;
}

static void http_send(socket_t client, int status, const char *status_text, const char *content_type, const char *body, bool auth) {
    char header[1024];
    snprintf(header, sizeof(header),
             "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n%s\r\n",
             status, status_text, content_type, strlen(body),
             auth ? "WWW-Authenticate: Basic realm=\"sleep-on-lan\"\r\n" : "");
    send(client, header, (int)strlen(header), 0);
    send(client, body, (int)strlen(body), 0);
}

static void strip_query(char *path) {
    char *q = strchr(path, '?');
    if (q) {
        *q = '\0';
    }
}

static void url_decode(char *s) {
    char *d = s;
    for (; *s; s++, d++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = {s[1], s[2], 0};
            *d = (char)strtol(hex, NULL, 16);
            s += 2;
        } else if (*s == '+') {
            *d = ' ';
        } else {
            *d = *s;
        }
    }
    *d = '\0';
}

static void handle_http_client(Config *cfg, socket_t client) {
    char req[HTTP_BUF + 1];
    int n = recv(client, req, HTTP_BUF, 0);
    if (n <= 0) {
        close_socket(client);
        return;
    }
    req[n] = '\0';
    if (!check_auth(cfg, req)) {
        http_send(client, 401, "Unauthorized", "text/plain", "Unauthorized\n", true);
        close_socket(client);
        return;
    }
    char method[16] = {0};
    char path[512] = {0};
    sscanf(req, "%15s %511s", method, path);
    bool json = wants_json(cfg, path);
    char route[512];
    snprintf(route, sizeof(route), "%s", path);
    strip_query(route);
    url_decode(route);
    char body[8192] = {0};
    int status = 200;
    const char *status_text = "OK";
    if (strcmp(method, "GET") != 0) {
        status = 405;
        status_text = "Method Not Allowed";
        snprintf(body, sizeof(body), "Method Not Allowed\n");
    } else if (strcmp(route, "/") == 0) {
        render_root(cfg, body, sizeof(body), json);
    } else if (strcmp(route, "/quit") == 0 || strcmp(route, "/quit/") == 0) {
        render_operation(body, sizeof(body), "quit", true, json);
        g_running = 0;
    } else if (strcmp(route, "/state/local/online") == 0 || strcmp(route, "/state/local/online/") == 0) {
        snprintf(body, sizeof(body), "true");
    } else if (strcmp(route, "/state/local") == 0 || strcmp(route, "/state/local/") == 0) {
        if (json) {
            snprintf(body, sizeof(body), "{\"host\":\"localhost\",\"state\":\"online\"}");
        } else {
            snprintf(body, sizeof(body), "<state host=\"localhost\" state=\"online\"/>");
        }
    } else if (strncmp(route, "/state/ip/", 10) == 0) {
        const char *ip = route + 10;
        if (json) {
            snprintf(body, sizeof(body), "{\"host\":\"%s\",\"state\":\"unknown\"}", ip);
        } else {
            snprintf(body, sizeof(body), "<state host=\"%s\" state=\"unknown\"/>", ip);
        }
    } else if (strncmp(route, "/wol/", 5) == 0) {
        const char *mac = route + 5;
        bool ok = send_wol(cfg, mac);
        render_operation(body, sizeof(body), "wol", ok, json);
    } else {
        const char *operation = route[0] == '/' ? route + 1 : route;
        size_t len = strlen(operation);
        if (len > 0 && operation[len - 1] == '/') {
            ((char *)operation)[len - 1] = '\0';
        }
        Command *cmd = find_command(cfg, operation);
        if (cmd) {
            long delay = cfg->delay_before_commands.active ? cfg->delay_before_commands.delay_ms : 0;
            execute_command_async(cmd, delay);
            render_operation(body, sizeof(body), operation, true, json);
        } else {
            status = 404;
            status_text = "Not Found";
            snprintf(body, sizeof(body), "Not Found\n");
        }
    }
    http_send(client, status, status_text, json ? "application/json" : "application/xml", body, false);
    close_socket(client);
}

static socket_t tcp_socket_bind(int port) {
    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET_T) {
        return INVALID_SOCKET_T;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close_socket(fd);
        return INVALID_SOCKET_T;
    }
    if (listen(fd, 16) < 0) {
        close_socket(fd);
        return INVALID_SOCKET_T;
    }
    return fd;
}

static THREAD_RET http_listener_thread(void *arg) {
    ListenerArgs *args = (ListenerArgs *)arg;
    Config *cfg = args->config;
    int port = args->port;
    free(args);
    socket_t fd = tcp_socket_bind(port);
    if (fd == (socket_t)-1) {
        log_msg("ERROR", "Unable to bind HTTP port %d: %s", port, socket_error_message());
        if (cfg->exit_if_port_used) {
            g_running = 0;
        }
        return THREAD_RETURN;
    }
    log_msg("INFO", "Listening for HTTP requests on port %d", port);
    while (g_running) {
        struct sockaddr_in remote;
        socket_len_t remote_len = (socket_len_t)sizeof(remote);
        socket_t client = accept(fd, (struct sockaddr *)&remote, &remote_len);
        if (client == (socket_t)-1) {
            continue;
        }
        handle_http_client(cfg, client);
    }
    close_socket(fd);
    return THREAD_RETURN;
}

static bool start_thread(thread_t *thread, thread_fn_t fn, ListenerArgs *args) {
#ifdef _WIN32
    *thread = _beginthreadex(NULL, 0, fn, args, 0, NULL);
    return *thread != 0;
#else
    return pthread_create(thread, NULL, fn, args) == 0;
#endif
}

static void detach_thread(thread_t thread) {
#ifdef _WIN32
    CloseHandle((HANDLE)thread);
#else
    pthread_detach(thread);
#endif
}

static void print_default_config(FILE *out) {
    fprintf(out,
            "{\n"
            "  \"Listeners\": [\n"
            "    \"UDP:7\",\n"
            "    \"UDP:9\",\n"
            "    \"HTTP:8009\"\n"
            "  ],\n"
            "  \"LogLevel\": \"INFO\",\n"
            "  \"BroadcastIP\": \"192.168.255.255\",\n"
            "  \"HTTPOutput\": \"XML\",\n"
            "  \"ExitIfAnyPortIsAlreadyUsed\": false,\n"
            "  \"Auth\": {\n"
            "    \"Login\": \"\",\n"
            "    \"Password\": \"\"\n"
            "  },\n"
            "  \"AvoidDualUDPSending\": {\n"
            "    \"Active\": false,\n"
            "    \"Delay\": \"100ms\"\n"
            "  },\n"
            "  \"DelayBeforeCommands\": {\n"
            "    \"Active\": true,\n"
            "    \"Delay\": \"500ms\"\n"
            "  },\n"
            "  \"Commands\": [\n"
            "    {\n"
            "      \"Operation\": \"sleep\",\n"
            "      \"Type\": \"external\",\n"
            "      \"Default\": true,\n"
#ifdef _WIN32
            "      \"Command\": \"rundll32.exe powrprof.dll,SetSuspendState 0,1,0\"\n"
#elif defined(__APPLE__)
            "      \"Command\": \"pmset sleepnow\"\n"
#else
            "      \"Command\": \"systemctl suspend\"\n"
#endif
            "    }\n"
            "  ]\n"
            "}\n");
}

static void usage(FILE *out) {
    fprintf(out,
            "Sleep-On-LAN %s\n\n"
            "Usage:\n"
            "  sol [--config FILE] [--verbose] [run]\n"
            "  sol [--config FILE] generate-configuration\n"
            "  sol --version\n"
#ifdef _WIN32
            "  sol service\n"
            "  sol install|uninstall|start|stop|restart|status\n"
#endif
            "\n"
            "Options:\n"
            "  -c, --config FILE   Configuration file to use\n"
            "  -v, --verbose       Enable extra startup logging\n"
            "  -h, --help          Show this help\n",
            SOL_VERSION);
}

static int generate_config_file(const char *config_arg) {
    if (config_arg) {
        FILE *f = fopen(config_arg, "rb");
        if (f) {
            fclose(f);
            log_msg("ERROR", "Refusing to overwrite existing file %s", config_arg);
            return 1;
        }
        f = fopen(config_arg, "wb");
        if (!f) {
            log_msg("ERROR", "Unable to write %s: %s", config_arg, strerror(errno));
            return 1;
        }
        print_default_config(f);
        fclose(f);
        log_msg("INFO", "Wrote default configuration to %s", config_arg);
    } else {
        print_default_config(stdout);
    }
    return 0;
}

static int run_configured_server(const char *config_arg, bool verbose) {
    g_running = 1;
    g_udp_action_pending = 0;
    Config cfg;
    init_default_config(&cfg);
    cfg.verbose = verbose;

    const char *config_path = find_config_path(config_arg);
    if (config_path) {
        if (!load_config(&cfg, config_path)) {
            log_msg("ERROR", "Failed to load configuration from %s", config_path);
            return 1;
        }
    } else {
        log_msg("INFO", "No configuration file found, using built-in defaults");
    }
    load_local_macs();
    log_msg("INFO", "sleep-on-lan %s starting", SOL_VERSION);
    for (int i = 0; i < g_local_mac_count; i++) {
        log_msg("INFO", "Interface %s MAC %s reversed %s",
                g_local_macs[i].name, g_local_macs[i].text, g_local_macs[i].reversed);
    }
    for (int i = 0; i < cfg.command_count; i++) {
        log_msg("INFO", "Command /%s%s: %s",
                cfg.commands[i].operation, cfg.commands[i].is_default ? " (default)" : "",
                cfg.commands[i].command);
    }
    for (int i = 0; i < cfg.listener_count; i++) {
        ListenerArgs *args = (ListenerArgs *)calloc(1, sizeof(*args));
        if (!args) {
            continue;
        }
        args->config = &cfg;
        args->port = cfg.listeners[i].port;
        thread_t thread = 0;
        bool ok = false;
        if (strcmp(cfg.listeners[i].type, "UDP") == 0) {
            ok = start_thread(&thread, udp_listener_thread, args);
        } else if (strcmp(cfg.listeners[i].type, "HTTP") == 0) {
            ok = start_thread(&thread, http_listener_thread, args);
        }
        if (ok) {
            detach_thread(thread);
        } else {
            free(args);
            log_msg("ERROR", "Unable to start %s listener on port %d",
                    cfg.listeners[i].type, cfg.listeners[i].port);
            if (cfg.exit_if_port_used) {
                return 1;
            }
        }
    }
    while (g_running) {
        sleep_ms(250);
    }
    log_msg("INFO", "Shutting down");
    return 0;
}

#ifdef _WIN32
typedef enum {
    WINDOWS_ACTION_NONE,
    WINDOWS_ACTION_SERVICE,
    WINDOWS_ACTION_INSTALL,
    WINDOWS_ACTION_UNINSTALL,
    WINDOWS_ACTION_START,
    WINDOWS_ACTION_STOP,
    WINDOWS_ACTION_RESTART,
    WINDOWS_ACTION_STATUS
} WindowsAction;

static const char *win32_error_message(void) {
    static char buf[128];
    snprintf(buf, sizeof(buf), "Win32 error %lu", GetLastError());
    return buf;
}

static bool windows_programdata_dir(char *out, size_t out_size) {
    char program_data[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("ProgramData", program_data, sizeof(program_data));
    if (n == 0 || n >= sizeof(program_data)) {
        snprintf(program_data, sizeof(program_data), "C:\\ProgramData");
    }
    int written = snprintf(out, out_size, "%s\\SleepOnLan", program_data);
    return written > 0 && (size_t)written < out_size;
}

static bool ensure_windows_programdata_config(void) {
    char dir[512];
    char config_path[512];
    if (!windows_programdata_dir(dir, sizeof(dir)) ||
        !windows_programdata_config_path(config_path, sizeof(config_path))) {
        return false;
    }
    DWORD attrs = GetFileAttributesA(dir);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryA(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            log_msg("ERROR", "Unable to create %s: %s", dir, win32_error_message());
            return false;
        }
    }
    FILE *f = fopen(config_path, "rb");
    if (f) {
        fclose(f);
        return true;
    }
    f = fopen(config_path, "wb");
    if (!f) {
        log_msg("ERROR", "Unable to create %s", config_path);
        return false;
    }
    print_default_config(f);
    fclose(f);
    log_msg("INFO", "Created default configuration at %s", config_path);
    return true;
}

static void set_service_status(DWORD current_state, DWORD win32_exit_code, DWORD wait_hint) {
    static DWORD checkpoint = 1;
    if (!g_service_status_handle) {
        return;
    }
    g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_service_status.dwCurrentState = current_state;
    g_service_status.dwWin32ExitCode = win32_exit_code;
    g_service_status.dwWaitHint = wait_hint;
    g_service_status.dwControlsAccepted = 0;
    if (current_state == SERVICE_RUNNING) {
        g_service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    }
    if (current_state == SERVICE_START_PENDING || current_state == SERVICE_STOP_PENDING) {
        g_service_status.dwCheckPoint = checkpoint++;
    } else {
        g_service_status.dwCheckPoint = 0;
    }
    SetServiceStatus(g_service_status_handle, &g_service_status);
}

static DWORD WINAPI service_control_handler(DWORD control, DWORD event_type, LPVOID event_data, LPVOID context) {
    (void)event_type;
    (void)event_data;
    (void)context;
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        set_service_status(SERVICE_STOP_PENDING, NO_ERROR, 3000);
        g_running = 0;
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

static VOID WINAPI service_main(DWORD argc, LPSTR *argv) {
    (void)argc;
    (void)argv;
    g_running_as_service = true;
    g_service_status_handle = RegisterServiceCtrlHandlerExA(SOL_SERVICE_NAME, service_control_handler, NULL);
    if (!g_service_status_handle) {
        windows_event_log(EVENTLOG_ERROR_TYPE, "Unable to register service control handler");
        return;
    }
    set_service_status(SERVICE_START_PENDING, NO_ERROR, 3000);
    set_service_status(SERVICE_RUNNING, NO_ERROR, 0);
    int rc = run_configured_server(g_service_config_arg, false);
    set_service_status(SERVICE_STOPPED, rc == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR, 0);
}

static int dispatch_windows_service(bool explicit_service) {
    SERVICE_TABLE_ENTRYA table[] = {
        { (LPSTR)SOL_SERVICE_NAME, service_main },
        { NULL, NULL }
    };
    if (StartServiceCtrlDispatcherA(table)) {
        return 1;
    }
    DWORD error = GetLastError();
    if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT && !explicit_service) {
        return 0;
    }
    log_msg("ERROR", "Unable to start service dispatcher: Win32 error %lu", error);
    return -1;
}

static bool current_executable_path(char *out, size_t out_size) {
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)out_size);
    return n > 0 && n < out_size;
}

static SC_HANDLE open_service_manager(DWORD access) {
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, access);
    if (!scm) {
        log_msg("ERROR", "Unable to open Service Control Manager: %s", win32_error_message());
    }
    return scm;
}

static int windows_install_service(void) {
    if (!ensure_windows_programdata_config()) {
        return 1;
    }
    char exe[MAX_PATH];
    char bin_path[MAX_PATH + 4];
    if (!current_executable_path(exe, sizeof(exe))) {
        log_msg("ERROR", "Unable to resolve executable path: %s", win32_error_message());
        return 1;
    }
    snprintf(bin_path, sizeof(bin_path), "\"%s\"", exe);
    SC_HANDLE scm = open_service_manager(SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        return 1;
    }
    SC_HANDLE service = CreateServiceA(
        scm,
        SOL_SERVICE_NAME,
        SOL_SERVICE_DISPLAY_NAME,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        bin_path,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL);
    if (!service) {
        DWORD error = GetLastError();
        if (error == ERROR_SERVICE_EXISTS) {
            service = OpenServiceA(scm, SOL_SERVICE_NAME, SERVICE_CHANGE_CONFIG);
            if (!service) {
                log_msg("ERROR", "Service %s exists but could not be opened: %s",
                        SOL_SERVICE_NAME, win32_error_message());
                CloseServiceHandle(scm);
                return 1;
            }
            if (!ChangeServiceConfigA(
                    service,
                    SERVICE_WIN32_OWN_PROCESS,
                    SERVICE_AUTO_START,
                    SERVICE_ERROR_NORMAL,
                    bin_path,
                    NULL,
                    NULL,
                    NULL,
                    NULL,
                    NULL,
                    SOL_SERVICE_DISPLAY_NAME)) {
                log_msg("ERROR", "Unable to update service: %s", win32_error_message());
                CloseServiceHandle(service);
                CloseServiceHandle(scm);
                return 1;
            }
            log_msg("INFO", "Updated %s service using %s", SOL_SERVICE_NAME, bin_path);
        } else {
            log_msg("ERROR", "Unable to create service: Win32 error %lu", error);
            CloseServiceHandle(scm);
            return 1;
        }
    } else {
        log_msg("INFO", "Installed %s service using %s", SOL_SERVICE_NAME, bin_path);
    }
    SC_ACTION actions[1];
    actions[0].Type = SC_ACTION_RESTART;
    actions[0].Delay = 60000;
    SERVICE_FAILURE_ACTIONSA failure_actions;
    memset(&failure_actions, 0, sizeof(failure_actions));
    failure_actions.dwResetPeriod = 86400;
    failure_actions.cActions = 1;
    failure_actions.lpsaActions = actions;
    ChangeServiceConfig2A(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure_actions);
    SERVICE_DESCRIPTIONA description;
    description.lpDescription = (LPSTR)SOL_SERVICE_DESCRIPTION;
    ChangeServiceConfig2A(service, SERVICE_CONFIG_DESCRIPTION, &description);
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return 0;
}

static int windows_start_service(void) {
    SC_HANDLE scm = open_service_manager(SC_MANAGER_CONNECT);
    if (!scm) {
        return 1;
    }
    SC_HANDLE service = OpenServiceA(scm, SOL_SERVICE_NAME, SERVICE_START);
    if (!service) {
        log_msg("ERROR", "Unable to open service: %s", win32_error_message());
        CloseServiceHandle(scm);
        return 1;
    }
    BOOL ok = StartServiceA(service, 0, NULL);
    if (!ok && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        log_msg("ERROR", "Unable to start service: %s", win32_error_message());
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return 1;
    }
    log_msg("INFO", "Service %s is running", SOL_SERVICE_NAME);
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return 0;
}

static int windows_stop_service(void) {
    SC_HANDLE scm = open_service_manager(SC_MANAGER_CONNECT);
    if (!scm) {
        return 1;
    }
    SC_HANDLE service = OpenServiceA(scm, SOL_SERVICE_NAME, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!service) {
        log_msg("ERROR", "Unable to open service: %s", win32_error_message());
        CloseServiceHandle(scm);
        return 1;
    }
    SERVICE_STATUS status;
    BOOL ok = ControlService(service, SERVICE_CONTROL_STOP, &status);
    if (!ok && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
        log_msg("ERROR", "Unable to stop service: %s", win32_error_message());
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return 1;
    }
    log_msg("INFO", "Service %s stop requested", SOL_SERVICE_NAME);
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return 0;
}

static int windows_uninstall_service(void) {
    SC_HANDLE scm = open_service_manager(SC_MANAGER_CONNECT);
    if (!scm) {
        return 1;
    }
    SC_HANDLE service = OpenServiceA(scm, SOL_SERVICE_NAME, DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!service) {
        log_msg("ERROR", "Unable to open service: %s", win32_error_message());
        CloseServiceHandle(scm);
        return 1;
    }
    SERVICE_STATUS status;
    ControlService(service, SERVICE_CONTROL_STOP, &status);
    if (!DeleteService(service)) {
        log_msg("ERROR", "Unable to delete service: %s", win32_error_message());
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return 1;
    }
    log_msg("INFO", "Uninstalled %s service", SOL_SERVICE_NAME);
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return 0;
}

static const char *service_state_name(DWORD state) {
    switch (state) {
        case SERVICE_STOPPED: return "stopped";
        case SERVICE_START_PENDING: return "start-pending";
        case SERVICE_STOP_PENDING: return "stop-pending";
        case SERVICE_RUNNING: return "running";
        case SERVICE_CONTINUE_PENDING: return "continue-pending";
        case SERVICE_PAUSE_PENDING: return "pause-pending";
        case SERVICE_PAUSED: return "paused";
        default: return "unknown";
    }
}

static int windows_service_status(void) {
    SC_HANDLE scm = open_service_manager(SC_MANAGER_CONNECT);
    if (!scm) {
        return 1;
    }
    SC_HANDLE service = OpenServiceA(scm, SOL_SERVICE_NAME, SERVICE_QUERY_STATUS);
    if (!service) {
        log_msg("ERROR", "Unable to open service: %s", win32_error_message());
        CloseServiceHandle(scm);
        return 1;
    }
    SERVICE_STATUS_PROCESS status;
    DWORD needed = 0;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status), &needed)) {
        log_msg("ERROR", "Unable to query service: %s", win32_error_message());
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return 1;
    }
    printf("%s: %s\n", SOL_SERVICE_NAME, service_state_name(status.dwCurrentState));
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return 0;
}

static int run_windows_action(WindowsAction action) {
    switch (action) {
        case WINDOWS_ACTION_INSTALL:
            return windows_install_service();
        case WINDOWS_ACTION_UNINSTALL:
            return windows_uninstall_service();
        case WINDOWS_ACTION_START:
            return windows_start_service();
        case WINDOWS_ACTION_STOP:
            return windows_stop_service();
        case WINDOWS_ACTION_RESTART:
            if (windows_stop_service() != 0) {
                return 1;
            }
            sleep_ms(1000);
            return windows_start_service();
        case WINDOWS_ACTION_STATUS:
            return windows_service_status();
        case WINDOWS_ACTION_SERVICE:
        case WINDOWS_ACTION_NONE:
            break;
    }
    return 0;
}
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    const char *config_arg = NULL;
    bool verbose = false;
    bool generate = false;
    bool foreground_run = false;
#ifdef _WIN32
    WindowsAction windows_action = WINDOWS_ACTION_NONE;
#endif
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                usage(stderr);
                return 2;
            }
            config_arg = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("sleep-on-lan %s\n", SOL_VERSION);
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "generate-configuration") == 0) {
            generate = true;
#ifdef _WIN32
        } else if (strcmp(argv[i], "service") == 0) {
            windows_action = WINDOWS_ACTION_SERVICE;
        } else if (strcmp(argv[i], "install") == 0) {
            windows_action = WINDOWS_ACTION_INSTALL;
        } else if (strcmp(argv[i], "uninstall") == 0) {
            windows_action = WINDOWS_ACTION_UNINSTALL;
        } else if (strcmp(argv[i], "start") == 0) {
            windows_action = WINDOWS_ACTION_START;
        } else if (strcmp(argv[i], "stop") == 0) {
            windows_action = WINDOWS_ACTION_STOP;
        } else if (strcmp(argv[i], "restart") == 0) {
            windows_action = WINDOWS_ACTION_RESTART;
        } else if (strcmp(argv[i], "status") == 0) {
            windows_action = WINDOWS_ACTION_STATUS;
#endif
        } else if (strcmp(argv[i], "run") == 0) {
            foreground_run = true;
        } else {
            usage(stderr);
            return 2;
        }
    }
    if (generate) {
        return generate_config_file(config_arg);
    }
#ifdef _WIN32
    if (windows_action != WINDOWS_ACTION_NONE && windows_action != WINDOWS_ACTION_SERVICE) {
        int rc = run_windows_action(windows_action);
        WSACleanup();
        return rc;
    }
    g_service_config_arg = config_arg;
    if (windows_action == WINDOWS_ACTION_SERVICE) {
        int service_result = dispatch_windows_service(true);
        WSACleanup();
        return service_result == 1 ? 0 : 1;
    }
    if (!foreground_run) {
        int service_result = dispatch_windows_service(false);
        if (service_result == 1) {
            WSACleanup();
            return 0;
        }
        if (service_result < 0) {
            WSACleanup();
            return 1;
        }
    }
#else
    (void)foreground_run;
#endif
    int rc = run_configured_server(config_arg, verbose);
#ifdef _WIN32
    WSACleanup();
#endif
    return rc;
}
