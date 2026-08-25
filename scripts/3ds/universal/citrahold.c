/*
 * citrahold.c — Citrahold-compatible save and extdata sync for Checkpoint.
 *
 * Install on a 3DS at /3ds/Checkpoint/scripts/universal/citrahold.c.
 * When bundled with Checkpoint, setup instructions live in scripts/citrahold.md.
 * The first run selects Official or Custom mode. The selection is persisted;
 * Configuration is the place to change modes later.
 *
 * State and temporary request files are stored under app_root()/config.
 * A passphrase is optional, but device_seal encrypts the state in either case.
 * Debug logging is disabled by default and never records credentials or save
 * contents.
 *
 * Upload payloads are written to a temporary SD-card file and streamed through
 * Checkpoint's native HTTP helper. Downloads are enumerated and then received
 * file-by-file with HTTP ranges, keeping Picoc response allocations bounded.
 * Temporary upload files are unlinked on startup and after each request;
 * incomplete download folders are removed before returning to the menu.
 */

#include <checkpoint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ROOTN 512
#define URLN 512
#define TOKENN 96
#define PASSN 64
#define BODYN 24000
#define DOWNLOAD_RESPONSE_LIMIT 65536
#define DOWNLOAD_CHUNK 524288
#define FILE_LIMIT 4096
#define B64_CHUNK 1536
#define UPLOAD_ERR_READ 1
#define UPLOAD_ERR_TEMP 2
#define PATHN 1024
#define IDN 128
#define TITLEIDN 17
#define MAXMAP 16
#define MAXREMOTE 8
#define MAXPICK 64
#define ROWN 256

#define OFFICIAL_URL "https://api.citrahold.com"

char g_root[ROOTN];
char g_config_dir[PATHN];
char g_vault[PATHN];
char g_log_dir[PATHN];
char g_log_path[PATHN];
char g_mode[16];
char g_url[URLN];
char g_official_token[TOKENN];
char g_custom_token[TOKENN];
char g_pass[PASSN];
int g_delete_after;
int g_debug;

int g_map_type[MAXMAP];
char g_map_title[MAXMAP][TITLEIDN];
char g_map_game[MAXMAP][IDN];
int g_map_count;
int g_remote_type[MAXREMOTE];
char g_remote_game[MAXREMOTE][IDN];
int g_remote_count;

char g_body[BODYN];
char g_upload_payload[PATHN];
char g_vault_tmp[PATHN];
char g_vault_old[PATHN];
int g_upload_error;
int g_upload_total;
int g_upload_done;

char* active_url(void);
char* active_token(void);
void cache_remote_games(int type, char* names, int count);
int json_string_field(struct JSON* root, char* key, char* dst, int size);
int is_regular_file(char* path);

void log_debug(char* message)
{
    if (g_debug) {
        FILE* f;
        sd_mkdirs(g_log_dir);
        f = fopen(g_log_path, "ab");
        if (f != NULL) {
            fputs(message, f);
            fputc('\n', f);
            fclose(f);
        }
        script_log(message);
    }
}

void init_paths(void)
{
    char* root = app_root();
    strcpy(g_root, root);
    sprintf(g_config_dir, "%s/config", root);
    sprintf(g_vault, "%s/config/citrahold.vault", root);
    sprintf(g_log_dir, "%s/logs/citrahold", root);
    sprintf(g_log_path, "%s/logs/citrahold/citrahold.log", root);
    sprintf(g_upload_payload, "%s/config/citrahold-upload-payload.json", root);
    sprintf(g_vault_tmp, "%s/config/citrahold.vault.tmp", root);
    sprintf(g_vault_old, "%s/config/citrahold.vault.old", root);
    sd_mkdirs(g_log_dir);
    free(root);
    /* A power loss can leave the streamed request body behind. Never retain it. */
    unlink(g_upload_payload);
    /* 3DS rename() does not replace an existing entry; recover an interrupted swap. */
    if (!sd_exists(g_vault) && sd_exists(g_vault_old)) rename(g_vault_old, g_vault);
    if (sd_exists(g_vault)) unlink(g_vault_old);
    unlink(g_vault_tmp);
    g_mode[0] = '\0';
    g_url[0] = '\0';
    g_official_token[0] = '\0';
    g_custom_token[0] = '\0';
    g_pass[0] = '\0';
    g_delete_after = 0;
    g_debug = 0;
    g_map_count = 0;
}

char* slurp_n(char* path, int* size)
{
    FILE* f;
    int bytes;
    int got;
    char* data;

    size[0] = 0;
    f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (bytes <= 0 || bytes > BODYN) {
        fclose(f);
        return NULL;
    }
    data = malloc(bytes + 1);
    if (data == NULL) {
        fclose(f);
        return NULL;
    }
    got = fread(data, 1, bytes, f);
    fclose(f);
    data[got] = '\0';
    size[0] = got;
    return data;
}

int append_text(char* dst, char* src, int limit)
{
    int at = strlen(dst);
    int i = 0;
    while (src[i] != '\0') {
        if (at + 1 >= limit) return 0;
        dst[at] = src[i];
        at = at + 1;
        i = i + 1;
    }
    dst[at] = '\0';
    return 1;
}

int append_json_string(char* dst, char* value, int limit)
{
    int at = strlen(dst);
    int i = 0;
    while (value[i] != '\0') {
        char c = value[i];
        if (c == '\\' || c == '"') {
            if (at + 2 >= limit) return 0;
            dst[at] = '\\';
            dst[at + 1] = c;
            at = at + 2;
        }
        else {
            if (at + 1 >= limit) return 0;
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            dst[at] = c;
            at = at + 1;
        }
        i = i + 1;
    }
    dst[at] = '\0';
    return 1;
}

int line_value(char* text, char* key, char* out, int outn)
{
    char* p = text;
    int keyn = strlen(key);
    int i;
    while (p != NULL && p[0] != '\0') {
        if (strncmp(p, key, keyn) == 0 && p[keyn] == '=') {
            p = p + keyn + 1;
            i = 0;
            while (p[i] != '\0' && p[i] != '\n' && i < outn - 1) {
                out[i] = p[i];
                i = i + 1;
            }
            out[i] = '\0';
            return 1;
        }
        p = strchr(p, '\n');
        if (p != NULL) p = p + 1;
    }
    out[0] = '\0';
    return 0;
}

void parse_maps(char* text)
{
    char* p = text;
    char line[PATHN];
    char* a;
    char* b;
    char* c;
    int n;

    g_map_count = 0;
    while (p != NULL && g_map_count < MAXMAP) {
        p = strstr(p, "map=");
        if (p == NULL) break;
        n = 0;
        while (p[n] != '\0' && p[n] != '\n' && n < PATHN - 1) {
            line[n] = p[n];
            n = n + 1;
        }
        line[n] = '\0';
        a = strtok(line, "|");
        b = strtok(NULL, "|");
        c = strtok(NULL, "|");
        if (a != NULL && b != NULL && c != NULL) {
            if (strncmp(a, "map=save", 8) == 0 || strncmp(a, "map=extdata", 11) == 0) {
                g_map_type[g_map_count] = (strncmp(a, "map=extdata", 11) == 0) ? 1 : 0;
                strncpy(g_map_title[g_map_count], b, TITLEIDN - 1);
                g_map_title[g_map_count][TITLEIDN - 1] = '\0';
                strncpy(g_map_game[g_map_count], c, IDN - 1);
                g_map_game[g_map_count][IDN - 1] = '\0';
                g_map_count = g_map_count + 1;
            }
        }
        p = strchr(p, '\n');
        if (p != NULL) p = p + 1;
    }
}

void parse_remote_games(char* text)
{
    char* p = text;
    char line[PATHN];
    char* a;
    char* b;
    int n;

    g_remote_count = 0;
    while (p != NULL && g_remote_count < MAXREMOTE) {
        p = strstr(p, "remote=");
        if (p == NULL) break;
        n = 0;
        while (p[n] != '\0' && p[n] != '\n' && n < PATHN - 1) {
            line[n] = p[n];
            n = n + 1;
        }
        line[n] = '\0';
        a = strtok(line, "|");
        b = strtok(NULL, "|");
        if (a != NULL && b != NULL) {
            if (strcmp(a, "remote=save") == 0 || strcmp(a, "remote=extdata") == 0) {
                g_remote_type[g_remote_count] = (strcmp(a, "remote=extdata") == 0) ? 1 : 0;
                strncpy(g_remote_game[g_remote_count], b, IDN - 1);
                g_remote_game[g_remote_count][IDN - 1] = '\0';
                g_remote_count = g_remote_count + 1;
            }
        }
        p = strchr(p, '\n');
        if (p != NULL) p = p + 1;
    }
}

void reset_state(void)
{
    g_mode[0] = '\0';
    g_url[0] = '\0';
    g_official_token[0] = '\0';
    g_custom_token[0] = '\0';
    g_delete_after = 0;
    g_debug = 0;
    g_map_count = 0;
    g_remote_count = 0;
}

int state_from_plain(char* plain)
{
    char value[URLN];
    if (!line_value(plain, "mode", g_mode, 16)) return 0;
    line_value(plain, "url", g_url, URLN);
    line_value(plain, "official_token", g_official_token, TOKENN);
    line_value(plain, "custom_token", g_custom_token, TOKENN);
    line_value(plain, "delete_after", value, 16);
    g_delete_after = (strcmp(value, "1") == 0);
    line_value(plain, "debug", value, 16);
    g_debug = (strcmp(value, "1") == 0);
    parse_maps(plain);
    parse_remote_games(plain);
    return 1;
}

int state_to_plain(char* plain, int limit)
{
    int i;
    char line[PATHN];
    plain[0] = '\0';
    if (!append_text(plain, "mode=", limit) || !append_text(plain, g_mode, limit) || !append_text(plain, "\nurl=", limit) || !append_json_string(plain, g_url, limit)) return 0;
    if (!append_text(plain, "\nofficial_token=", limit) || !append_json_string(plain, g_official_token, limit)) return 0;
    if (!append_text(plain, "\ncustom_token=", limit) || !append_json_string(plain, g_custom_token, limit)) return 0;
    sprintf(line, "\ndelete_after=%d\ndebug=%d\n", g_delete_after, g_debug);
    if (!append_text(plain, line, limit)) return 0;
    for (i = 0; i < g_map_count; i++) {
        sprintf(line, "map=%s|%s|%s\n", g_map_type[i] ? "extdata" : "save", g_map_title[i], g_map_game[i]);
        if (!append_text(plain, line, limit)) return 0;
    }
    for (i = 0; i < g_remote_count; i++) {
        sprintf(line, "remote=%s|%s\n", g_remote_type[i] ? "extdata" : "save", g_remote_game[i]);
        if (!append_text(plain, line, limit)) return 0;
    }
    return 1;
}

void seal_error(int rc)
{
    char message[160];
    if (rc == -5) strcpy(message, "Wrong passphrase, wrong console, or damaged state.");
    else if (rc == -2) strcpy(message, "State was sealed by a newer Checkpoint.");
    else if (rc == -3) strcpy(message, "Not enough memory to open the state.");
    else if (rc == -4) strcpy(message, "The console has no available key source.");
    else strcpy(message, "Could not open encrypted Citrahold state.");
    script_log(message);
    gui_message(message);
}

int state_write(void)
{
    char plain[4096];
    char* blob = NULL;
    int blob_size = 0;
    int rc;
    int moved_old = 0;
    FILE* f;

    if (!state_to_plain(plain, 4096)) {
        gui_message("Citrahold settings are too large.");
        return 0;
    }
    rc = device_seal(plain, strlen(plain), g_pass, &blob, &blob_size);
    if (rc != 0) {
        seal_error(rc);
        return 0;
    }
    sd_mkdirs(g_config_dir);
    unlink(g_vault_tmp);
    f = fopen(g_vault_tmp, "wb");
    if (f == NULL) {
        free(blob);
        gui_message("Could not write the encrypted state.");
        return 0;
    }
    if (fwrite(blob, 1, blob_size, f) != blob_size) {
        fclose(f);
        unlink(g_vault_tmp);
        free(blob);
        gui_message("The encrypted state was written short.");
        return 0;
    }
    if (fclose(f) != 0) {
        unlink(g_vault_tmp);
        free(blob);
        gui_message("Could not commit the encrypted state.");
        return 0;
    }
    /* Keep the last good vault recoverable while installing the closed file. */
    if (sd_exists(g_vault_old) && unlink(g_vault_old) != 0) {
        unlink(g_vault_tmp);
        free(blob);
        gui_message("Could not prepare the encrypted state.");
        return 0;
    }
    if (sd_exists(g_vault)) {
        if (rename(g_vault, g_vault_old) != 0) {
            unlink(g_vault_tmp);
            free(blob);
            gui_message("Could not preserve the encrypted state.");
            return 0;
        }
        moved_old = 1;
    }
    if (rename(g_vault_tmp, g_vault) != 0) {
        if (moved_old) rename(g_vault_old, g_vault);
        unlink(g_vault_tmp);
        free(blob);
        gui_message("Could not commit the encrypted state.");
        return 0;
    }
    unlink(g_vault_old);
    free(blob);
    return 1;
}

int state_read(void)
{
    char* blob;
    char* plain = NULL;
    int blob_size;
    int plain_size = 0;
    int needs;
    int rc;

    blob = slurp_n(g_vault, &blob_size);
    if (blob == NULL) return 0;
    needs = seal_needs_passphrase(blob, blob_size);
    if (needs < 0) {
        free(blob);
        gui_message("The Citrahold state is not a valid sealed file.");
        return -1;
    }
    if (needs) {
        gui_keyboard(g_pass, "Enter Citrahold passphrase", PASSN);
        if (g_pass[0] == '\0') {
            free(blob);
            return -1;
        }
    }
    rc = device_unseal(blob, blob_size, g_pass, &plain, &plain_size);
    free(blob);
    if (rc != 0 || plain == NULL) {
        seal_error(rc);
        return -1;
    }
    if (!state_from_plain(plain)) {
        free(plain);
        gui_message("The Citrahold state is missing its mode.");
        return -1;
    }
    free(plain);
    return 1;
}

int choose_mode(void)
{
    char* choices[2];
    int pick;
    choices[0] = "Official Citrahold";
    choices[1] = "Custom Citrahold server";
    pick = gui_pick_one("Select Citrahold server mode", choices, 2);
    if (pick < 0) return 0;
    if (pick == 0) {
        strcpy(g_mode, "official");
        strcpy(g_url, OFFICIAL_URL);
    }
    else {
        strcpy(g_mode, "custom");
        gui_keyboard(g_url, "Enter Citrahold server URL", URLN);
        if (g_url[0] == '\0') return 0;
        while (g_url[strlen(g_url) - 1] == '/') g_url[strlen(g_url) - 1] = '\0';
    }
    return 1;
}

int valid_game_id(char* game)
{
    int i = 0;
    if (game[0] == '\0' || strlen(game) >= IDN) return 0;
    while (game[i] != '\0') {
        if (game[i] == '/' || game[i] == '\\' || game[i] == '"' || game[i] == '\n' || game[i] == '\r' || game[i] == '|') return 0;
        if (game[i] == '.' && game[i + 1] == '.') return 0;
        i = i + 1;
    }
    return 1;
}

int configure_first_run(void)
{
    char token[TOKENN];
    if (!choose_mode()) return 0;
    gui_keyboard(token, "Full token or shorthand token", TOKENN);
    if (token[0] == '\0') return 0;
    if (strlen(token) < 16) {
        char url[URLN];
        char body[256];
        char* out = NULL;
        int out_size = 0;
        int status;
        struct JSON* root;
        char exchanged[TOKENN];
        sprintf(url, "%s/getToken", active_url());
        sprintf(body, "{\"shorthandToken\":\"%s\"}", token);
        status = web_request("POST", url, "Content-Type: application/json", body, strlen(body), &out, &out_size, NULL);
        if (status != 200 || out == NULL) {
            if (out != NULL) free(out);
            gui_message("The shorthand token was rejected.");
            return 0;
        }
        root = json_new();
        json_parse(root, out);
        exchanged[0] = '\0';
        if (!json_string_field(root, "token", exchanged, TOKENN)) {
            json_delete(root);
            free(out);
            gui_message("The server returned an invalid token response.");
            return 0;
        }
        json_delete(root);
        free(out);
        strcpy(token, exchanged);
    }
    if (strcmp(g_mode, "official") == 0) strcpy(g_official_token, token);
    else strcpy(g_custom_token, token);
    if (gui_confirm("Add a passphrase to the encrypted state?")) {
        gui_keyboard(g_pass, "Enter passphrase", PASSN);
        if (g_pass[0] == '\0') return 0;
    }
    return state_write();
}

char* active_token(void)
{
    if (strcmp(g_mode, "official") == 0) return g_official_token;
    return g_custom_token;
}

char* active_url(void)
{
    if (strcmp(g_mode, "official") == 0) return OFFICIAL_URL;
    return g_url;
}

int api_call(char* endpoint, char* body, char** out, int* out_size)
{
    char url[URLN];
    char headers[128];
    char* response_headers = NULL;
    int status;
    sprintf(url, "%s%s", active_url(), endpoint);
    strcpy(headers, "Content-Type: application/json");
    status = web_request("POST", url, headers, body, strlen(body), out, out_size, &response_headers);
    if (response_headers != NULL) free(response_headers);
    if (g_debug) {
        char line[96];
        sprintf(line, "HTTP %d %s", status, endpoint);
        script_log(line);
    }
    return status;
}

/* Like api_call(), with extra newline-separated request headers. The caller
 * owns response_headers and must free it when it is non-NULL. */
int api_call_headers(char* endpoint, char* extra_headers, char* body, char** out, int* out_size, char** response_headers)
{
    char url[URLN];
    char headers[256];
    int status;
    sprintf(url, "%s%s", active_url(), endpoint);
    strcpy(headers, "Content-Type: application/json");
    if (extra_headers != NULL && extra_headers[0] != '\0') {
        strcat(headers, "\n");
        strcat(headers, extra_headers);
    }
    status = web_request("POST", url, headers, body, strlen(body), out, out_size, response_headers);
    if (g_debug) {
        char line[96];
        sprintf(line, "HTTP %d %s", status, endpoint);
        script_log(line);
    }
    return status;
}

int api_upload_file(char* endpoint, char* path, char** out, int* out_size)
{
    char url[URLN];
    char headers[128];
    char* response_headers = NULL;
    int status;
    sprintf(url, "%s%s", active_url(), endpoint);
    strcpy(headers, "Content-Type: application/json");
    status = web_upload_file("POST", url, headers, path, out, out_size, &response_headers);
    if (response_headers != NULL) free(response_headers);
    if (g_debug) {
        char line[96];
        sprintf(line, "HTTP %d %s", status, endpoint);
        script_log(line);
    }
    return status;
}

int json_string_field(struct JSON* root, char* key, char* dst, int size)
{
    struct JSON* field;
    char* value;
    if (!json_object_contains(root, key)) return 0;
    field = json_object_element(root, key);
    if (!json_is_string(field)) return 0;
    value = json_get_string(field);
    strncpy(dst, value, size - 1);
    dst[size - 1] = '\0';
    free(value);
    return 1;
}

int server_test(void)
{
    char* out = NULL;
    int n = 0;
    int status;
    status = api_call("/areyouawake", "{}", &out, &n);
    if (out != NULL) free(out);
    if (status == 200) {
        gui_message("Citrahold server is online.");
        return 1;
    }
    gui_message("Citrahold server could not be reached.");
    return 0;
}

void show_server_info(void)
{
    char* out = NULL;
    int n = 0;
    int status;
    int i;
    struct JSON* root;
    struct JSON* list;
    struct JSON* item;
    char* text;

    printf("Citrahold server: %s\n", active_url());
    log_debug("starting server information query");
    status = api_call("/softwareVersion", "{}", &out, &n);
    if (status != 200 || out == NULL) {
        log_debug("server information query failed");
        printf("Could not read Citrahold version or message of the day (HTTP %d).\n", status);
        if (out != NULL) free(out);
        return;
    }
    root = json_new();
    json_parse(root, out);
    if (json_is_valid(root) && json_object_contains(root, "3ds")) {
        list = json_object_element(root, "3ds");
        if (json_is_array(list) && json_array_size(list) > 0) {
            item = json_array_element(list, 0);
            if (json_is_string(item)) {
                text = json_get_string(item);
                printf("Server 3DS version: %s\n", text);
                free(text);
            }
        }
    }
    if (json_is_valid(root) && json_object_contains(root, "motd3ds")) {
        list = json_object_element(root, "motd3ds");
        if (json_is_array(list)) {
            for (i = 0; i < json_array_size(list); i++) {
                item = json_array_element(list, i);
                if (json_is_string(item)) {
                    text = json_get_string(item);
                    if (text[0] != '\0') printf("Message: %s\n", text);
                    free(text);
                }
            }
        }
    }
    json_delete(root);
    free(out);
}

int verify_token(void)
{
    char body[512];
    char* out = NULL;
    int n = 0;
    int status;
    char user[IDN];
    sprintf(body, "{\"token\":\"%s\"}", active_token());
    status = api_call("/getUserID", body, &out, &n);
    if (status != 200 || out == NULL) {
        if (out != NULL) free(out);
        gui_message("The Citrahold token was rejected.");
        return 0;
    }
    {
        struct JSON* root = json_new();
        json_parse(root, out);
        user[0] = '\0';
        if (!json_string_field(root, "userID", user, IDN)) {
            json_delete(root);
            free(out);
            gui_message("The server returned an invalid token response.");
            return 0;
        }
        json_delete(root);
    }
    free(out);
    {
        char line[180];
        sprintf(line, "Authenticated as %s", user);
        gui_message(line);
    }
    return 1;
}

/*
 * Keep the result buffer as a pointer parameter.  Picoc copies array-typed
 * parameters into a fixed 256-byte temporary buffer, so a MAXPICK x IDN
 * parameter asserts before this function can run.
 */
int remote_games(int type, char* names)
{
    char body[512];
    char* out = NULL;
    int n = 0;
    int status;
    int count = 0;
    int i;
    struct JSON* root;
    struct JSON* games;
    struct JSON* item;
    char endpoint[32];

    sprintf(body, "{\"token\":\"%s\"}", active_token());
    strcpy(endpoint, type ? "/getExtdata" : "/getSaves");
    log_debug("requesting remote game list");
    status = api_call(endpoint, body, &out, &n);
    log_debug("remote game list request returned");
    if (status != 200 || out == NULL || n < 0 || n > BODYN - 1) {
        if (out != NULL) free(out);
        return -1;
    }
    root = json_new();
    if (root == NULL) {
        free(out);
        return -1;
    }
    json_parse(root, out);
    log_debug("remote game list parsed");
    if (!json_is_valid(root) || !json_object_contains(root, "games")) {
        json_delete(root);
        free(out);
        return -1;
    }
    games = json_object_element(root, "games");
    if (games == NULL || !json_is_array(games)) {
        json_delete(root);
        free(out);
        return -1;
    }
    log_debug("remote game list array validated");
    count = json_array_size(games);
    if (count > MAXPICK) count = MAXPICK;
    for (i = 0; i < count; i++) {
        item = json_array_element(games, i);
        if (json_is_string(item)) {
            char* s = json_get_string(item);
            strncpy(names + i * IDN, s, IDN - 1);
            names[i * IDN + IDN - 1] = '\0';
            free(s);
        }
        else {
            names[i * IDN] = '\0';
        }
    }
    printf("Remote %s Game IDs: %d\n", type ? "extdata" : "save", count);
    for (i = 0; i < count; i++) printf("  %s\n", names + i * IDN);
    cache_remote_games(type, names, count);
    json_delete(root);
    free(out);
    return count;
}

int remote_has(char* names, int count, char* game)
{
    int i;
    for (i = 0; i < count; i++) if (strcmp(names + i * IDN, game) == 0) return 1;
    return 0;
}

void cache_remote_games(int type, char* names, int count)
{
    int kept = 0;
    int i;
    int changed = 0;
    int old_type[MAXREMOTE];
    char old_game[MAXREMOTE][IDN];
    int old_count = g_remote_count;

    for (i = 0; i < old_count; i++) {
        old_type[i] = g_remote_type[i];
        strcpy(old_game[i], g_remote_game[i]);
    }
    g_remote_count = 0;
    for (i = 0; i < old_count && g_remote_count < MAXREMOTE; i++) {
        if (old_type[i] != type) {
            g_remote_type[g_remote_count] = old_type[i];
            strcpy(g_remote_game[g_remote_count], old_game[i]);
            g_remote_count = g_remote_count + 1;
        }
    }
    for (i = 0; i < count && g_remote_count < MAXREMOTE; i++) {
        char* game = names + i * IDN;
        if (game[0] != '\0' && valid_game_id(game)) {
            g_remote_type[g_remote_count] = type;
            strcpy(g_remote_game[g_remote_count], game);
            g_remote_count = g_remote_count + 1;
            kept = kept + 1;
        }
    }
    if (old_count != g_remote_count) changed = 1;
    for (i = 0; !changed && i < g_remote_count; i++) {
        if (i >= old_count || old_type[i] != g_remote_type[i] || strcmp(old_game[i], g_remote_game[i]) != 0) changed = 1;
    }
    if (changed) state_write();
    printf("Cached %d remote %s Game ID%s.\n", kept, type ? "extdata" : "save", kept == 1 ? "" : "s");
}

int remote_timestamp(int type, char* game, char* result, int size)
{
    char body[512];
    char* out = NULL;
    int n = 0;
    int status;
    struct JSON* root;
    char endpoint[48];
    result[0] = '\0';
    sprintf(body, "{\"token\":\"%s\",\"game\":\"%s\"}", active_token(), game);
    strcpy(endpoint, type ? "/getExtdataLastUpdated" : "/getSavesLastUpdated");
    status = api_call(endpoint, body, &out, &n);
    if (status != 200 || out == NULL) {
        if (out != NULL) free(out);
        return 0;
    }
    root = json_new();
    json_parse(root, out);
    if (!json_string_field(root, "lastModified", result, size)) {
        json_delete(root);
        free(out);
        return 0;
    }
    json_delete(root);
    free(out);
    return 1;
}

int base64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int write_text(FILE* out, char* text)
{
    if (fputs(text, out) < 0) {
        g_upload_error = UPLOAD_ERR_TEMP;
        return 0;
    }
    return 1;
}

int write_json_string(FILE* out, char* value)
{
    int i;
    char c;
    if (fputc('"', out) == EOF) return 0;
    for (i = 0; value[i] != '\0'; i++) {
        c = value[i];
        if (c == '"' || c == '\\') {
            if (fputc('\\', out) == EOF || fputc(c, out) == EOF) return 0;
        }
        else if (c == '\n') {
            if (!write_text(out, "\\n")) return 0;
        }
        else if (c == '\r') {
            if (!write_text(out, "\\r")) return 0;
        }
        else if (c == '\t') {
            if (!write_text(out, "\\t")) return 0;
        }
        else if ((unsigned char)c < 32 || fputc(c, out) == EOF) return 0;
    }
    return fputc('"', out) != EOF;
}

int write_base64_file(FILE* out, char* path)
{
    FILE* in;
    unsigned char* raw;
    char* encoded;
    int read;
    int raw_pos;
    int encoded_pos;
    int a;
    int b;
    int c;
    in = fopen(path, "rb");
    if (in == NULL) {
        g_upload_error = UPLOAD_ERR_READ;
        printf("Upload could not read: %s\n", path);
        return 0;
    }
    raw = malloc(B64_CHUNK);
    encoded = malloc(B64_CHUNK / 3 * 4);
    if (raw == NULL || encoded == NULL) {
        if (raw != NULL) free(raw);
        if (encoded != NULL) free(encoded);
        fclose(in);
        g_upload_error = UPLOAD_ERR_TEMP;
        return 0;
    }
    while ((read = fread(raw, 1, B64_CHUNK, in)) > 0) {
        raw_pos = 0;
        encoded_pos = 0;
        while (raw_pos < read) {
            a = raw[raw_pos++];
            b = raw_pos < read ? raw[raw_pos++] : -1;
            c = raw_pos < read ? raw[raw_pos++] : -1;
            encoded[encoded_pos++] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[(a >> 2) & 63];
            encoded[encoded_pos++] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[((a & 3) << 4) | ((b < 0 ? 0 : b) >> 4)];
            encoded[encoded_pos++] = (b < 0) ? '=' : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[((b & 15) << 2) | ((c < 0 ? 0 : c) >> 6)];
            encoded[encoded_pos++] = (c < 0) ? '=' : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[c & 63];
        }
        if (fwrite(encoded, 1, encoded_pos, out) != encoded_pos) {
            g_upload_error = UPLOAD_ERR_TEMP;
            free(raw);
            free(encoded);
            fclose(in);
            return 0;
        }
        g_upload_done = g_upload_done + read;
        progress_set(0, g_upload_done);
    }
    if (ferror(in)) {
        g_upload_error = UPLOAD_ERR_READ;
        free(raw);
        free(encoded);
        fclose(in);
        return 0;
    }
    free(raw);
    free(encoded);
    fclose(in);
    return 1;
}

int measure_upload_tree(char* root, int* total)
{
    struct directory* d;
    FILE* f;
    int i;
    int size;
    char child[PATHN];
    if (is_regular_file(root)) {
        f = fopen(root, "rb");
        if (f == NULL || fseek(f, 0, SEEK_END) != 0) {
            if (f != NULL) fclose(f);
            g_upload_error = UPLOAD_ERR_READ;
            return 0;
        }
        size = ftell(f);
        fclose(f);
        if (size < 0 || size > 2147483647 - *total) {
            g_upload_error = UPLOAD_ERR_READ;
            return 0;
        }
        *total = *total + size;
        return 1;
    }
    d = read_directory(root);
    if (d == NULL) {
        g_upload_error = UPLOAD_ERR_READ;
        return 0;
    }
    for (i = 0; i < d->count; i++) {
        strncpy(child, d->files[i], PATHN - 1);
        child[PATHN - 1] = '\0';
        if (!measure_upload_tree(child, total)) {
            delete_directory(d);
            return 0;
        }
    }
    delete_directory(d);
    return 1;
}

int write_upload_entry(FILE* out, char* remote_path, char* local_path, int dummy)
{
    if (!write_text(out, "[") || !write_json_string(out, remote_path) || !write_text(out, ",")) return 0;
    if (dummy) {
        if (!write_json_string(out, "citraholdDirectoryDummy")) return 0;
    }
    else {
        if (fputc('"', out) == EOF || !write_base64_file(out, local_path) || fputc('"', out) == EOF) return 0;
    }
    return write_text(out, "]");
}

void relative_path(char* full, char* base, char* out, int size)
{
    int n = strlen(base);
    while (n > 1 && base[n - 1] == '/') n = n - 1;
    if (strncmp(full, base, n) == 0 && full[n] == '/') {
        int i = 0;
        while (full[n + 1 + i] != '\0' && i < size - 1) {
            out[i] = full[n + 1 + i];
            i = i + 1;
        }
        out[i] = '\0';
    }
    else {
        strncpy(out, full, size - 1);
    }
    out[size - 1] = '\0';
}

int is_regular_file(char* path)
{
    FILE* f = fopen(path, "rb");
    if (f == NULL) return 0;
    fclose(f);
    return 1;
}

void parent_path(char* path, char* out, int size)
{
    int i = strlen(path) - 1;
    while (i >= 0 && path[i] != '/') i = i - 1;
    if (i <= 0) {
        strncpy(out, path, size - 1);
        out[size - 1] = '\0';
        return;
    }
    if (i >= size) i = size - 1;
    strncpy(out, path, i);
    out[i] = '\0';
}

int remove_tree(char* path)
{
    struct directory* d;
    int i;
    char child[PATHN];
    if (is_regular_file(path)) return unlink(path) == 0;
    d = read_directory(path);
    if (d == NULL) return 0;
    for (i = 0; i < d->count; i++) {
        strncpy(child, d->files[i], PATHN - 1);
        child[PATHN - 1] = '\0';
        if (!remove_tree(child)) {
            delete_directory(d);
            return 0;
        }
    }
    delete_directory(d);
    return rmdir(path) == 0;
}

int write_tree(FILE* out, char* root, char* base, char* game, int* entries)
{
    struct directory* d;
    int i;
    char rel[PATHN];
    char remote[PATHN];
    char child[PATHN];
    char marker[PATHN];
    if (is_regular_file(root)) {
        relative_path(root, base, rel, PATHN);
        sprintf(remote, "%s/%s", game, rel);
        if (*entries > 0 && !write_text(out, ",")) return 0;
        if (!write_upload_entry(out, remote, root, 0)) return 0;
        *entries = *entries + 1;
        return 1;
    }
    d = read_directory(root);
    if (d == NULL) {
        g_upload_error = UPLOAD_ERR_READ;
        printf("Upload could not read folder: %s\n", root);
        return 0;
    }
    if (strcmp(root, base) != 0) {
        relative_path(root, base, rel, PATHN);
        sprintf(marker, "%s/citraholdDirectoryDummy", rel);
        sprintf(remote, "%s/%s", game, marker);
        if (*entries > 0 && !write_text(out, ",")) {
            delete_directory(d);
            return 0;
        }
        if (!write_upload_entry(out, remote, "", 1)) {
            delete_directory(d);
            return 0;
        }
        *entries = *entries + 1;
    }
    for (i = 0; i < d->count; i++) {
        strncpy(child, d->files[i], PATHN - 1);
        child[PATHN - 1] = '\0';
        if (!write_tree(out, child, base, game, entries)) {
            delete_directory(d);
            return 0;
        }
    }
    delete_directory(d);
    return 1;
}

int write_upload_payload(char* backup, char* game)
{
    FILE* out;
    int entries = 0;
    g_upload_total = 0;
    g_upload_done = 0;
    if (!measure_upload_tree(backup, &g_upload_total)) return 0;
    progress_begin(0, "Preparing upload payload", g_upload_total > 0 ? g_upload_total : 1);
    unlink(g_upload_payload);
    out = fopen(g_upload_payload, "wb");
    if (out == NULL) {
        g_upload_error = UPLOAD_ERR_TEMP;
        progress_end(0);
        return 0;
    }
    if (!write_text(out, "{\"token\":") || !write_json_string(out, active_token()) || !write_text(out, ",\"game\":") || !write_json_string(out, game) || !write_text(out, ",\"multi\":[") || !write_tree(out, backup, backup, game, &entries) || entries == 0 || !write_text(out, "]}")) {
        fclose(out);
        unlink(g_upload_payload);
        progress_end(0);
        return 0;
    }
    if (fclose(out) != 0) {
        g_upload_error = UPLOAD_ERR_TEMP;
        unlink(g_upload_payload);
        progress_end(0);
        return 0;
    }
    progress_set(0, g_upload_total);
    progress_end(0);
    return 1;
}

int choose_title(int type)
{
    char* names[MAXPICK];
    int indexes[MAXPICK];
    int count = 0;
    int total = titles_count();
    int i;
    int pick;
    for (i = 0; i < total && count < MAXPICK; i++) {
        if ((type == 0 && title_has_save(i)) || (type == 1 && title_has_extdata(i))) {
            indexes[count] = i;
            names[count] = title_name(i);
            count = count + 1;
        }
    }
    if (count == 0) {
        gui_message("No compatible titles were found.");
        return -1;
    }
    pick = gui_pick_one(type ? "Select extdata title" : "Select save title", names, count);
    for (i = 0; i < count; i++) free(names[i]);
    if (pick < 0) return -1;
    return indexes[pick];
}

int find_map(int type, char* title_id)
{
    int i;
    for (i = 0; i < g_map_count; i++) if (g_map_type[i] == type && strcmp(g_map_title[i], title_id) == 0) return i;
    return -1;
}

int find_game_map(int type, char* game)
{
    int i;
    for (i = 0; i < g_map_count; i++) if (g_map_type[i] == type && strcmp(g_map_game[i], game) == 0) return i;
    return -1;
}

void remove_mapping(int index)
{
    int i;
    for (i = index; i < g_map_count - 1; i++) {
        g_map_type[i] = g_map_type[i + 1];
        strcpy(g_map_title[i], g_map_title[i + 1]);
        strcpy(g_map_game[i], g_map_game[i + 1]);
    }
    g_map_count = g_map_count - 1;
}

void assignment_row(int type, char* game, char* row, int size)
{
    int map = find_game_map(type, game);
    if (map < 0) {
        snprintf(row, size, "(Unassigned) %s", game);
    }
    else {
        int idx = title_find(g_map_title[map]);
        char* name = idx >= 0 ? title_name(idx) : NULL;
        if (name != NULL) {
            snprintf(row, size, "%s -> %s", game, name);
            free(name);
        }
        else snprintf(row, size, "%s -> linked title", game);
    }
}

int choose_remote_game(int type, char* game)
{
    char* rows[MAXREMOTE + 1];
    int indexes[MAXREMOTE];
    int count = 0;
    int i;
    int pick;
    for (i = 0; i < g_remote_count; i++) if (g_remote_type[i] == type) {
        rows[count] = malloc(IDN + 64);
        assignment_row(type, g_remote_game[i], rows[count], IDN + 64);
        indexes[count] = i;
        count = count + 1;
    }
    rows[count] = "Enter a Game ID manually";
    pick = gui_pick_one(type ? "Select extdata Game ID" : "Select save Game ID", rows, count + 1);
    for (i = 0; i < count; i++) free(rows[i]);
    if (pick < 0) return 0;
    if (pick == count) {
        gui_keyboard(game, "Enter Citrahold Game ID", IDN);
        return valid_game_id(game);
    }
    strcpy(game, g_remote_game[indexes[pick]]);
    return 1;
}

void refresh_remote_games(void)
{
    char save_names[MAXPICK * IDN];
    char extdata_names[MAXPICK * IDN];
    int saves = remote_games(0, save_names);
    int extdata = remote_games(1, extdata_names);
    if (saves < 0 && extdata < 0) {
        gui_message("Could not query Citrahold Game IDs.");
        return;
    }
    gui_message("Server Game IDs were saved.\n(Unassigned) entries must be\nlinked to a Checkpoint title.");
}

void mappings_menu(void)
{
    char* choices[4];
    int choice;
    choices[0] = "Link Game ID to title";
    choices[1] = "Unlink Game ID from title";
    choices[2] = "Refresh server Game IDs";
    choices[3] = "Back";
    choice = gui_pick_one("Manage Game IDs", choices, 4);
    if (choice == 0) {
        int type;
        int idx;
        char* title_id_value;
        char game[IDN];
        int old_title;
        int old_game;
        char* type_choices[2];
        type_choices[0] = "Save";
        type_choices[1] = "Extdata";
        type = gui_pick_one("Mapping type", type_choices, 2);
        if (type < 0) return;
        idx = choose_title(type);
        if (idx < 0) return;
        title_id_value = title_id(idx);
        if (choose_remote_game(type, game)) {
            old_title = find_map(type, title_id_value);
            old_game = find_game_map(type, game);
            if (old_title >= 0 && old_title != old_game) {
                remove_mapping(old_title);
                if (old_game > old_title) old_game = old_game - 1;
            }
            if (old_game < 0 && g_map_count < MAXMAP) old_game = g_map_count++;
            if (old_game >= 0) {
                g_map_type[old_game] = type;
                strcpy(g_map_title[old_game], title_id_value);
                strcpy(g_map_game[old_game], game);
                if (state_write()) gui_message("Game ID linked to title.");
            }
            else gui_message("Too many Game ID links are configured.");
        }
        else {
            gui_message("Game IDs may not contain slashes, quotes, pipes, or '..'.");
        }
        free(title_id_value);
    }
    else if (choice == 1) {
        char* rows[MAXMAP];
        int i;
        int pick;
        int row_count = g_map_count;
        for (i = 0; i < g_map_count; i++) {
            rows[i] = malloc(IDN + 64);
            assignment_row(g_map_type[i], g_map_game[i], rows[i], IDN + 64);
        }
        if (g_map_count > 0) {
            pick = gui_pick_one("Unlink which Game ID?", rows, g_map_count);
            if (pick >= 0) {
                remove_mapping(pick);
                state_write();
            }
        }
        else gui_message("No Game IDs are linked yet.");
        for (i = 0; i < row_count; i++) free(rows[i]);
    }
    else if (choice == 2) refresh_remote_games();
}

int choose_mapping(int type)
{
    char* rows[MAXMAP];
    int indexes[MAXMAP];
    int count = 0;
    int i;
    int pick;
    for (i = 0; i < g_map_count; i++) if (g_map_type[i] == type) {
        rows[count] = malloc(IDN + TITLEIDN + 8);
        sprintf(rows[count], "%s", g_map_game[i]);
        indexes[count] = i;
        count = count + 1;
    }
    if (count == 0) {
        gui_message("No mapping exists for this data type.");
        return -1;
    }
    pick = gui_pick_one(type ? "Select extdata Game ID" : "Select save Game ID", rows, count);
    for (i = 0; i < count; i++) free(rows[i]);
    if (pick < 0) return -1;
    return indexes[pick];
}

int choose_backup(int idx, int type, char* path, int size)
{
    char* base = title_backup_path(idx, type);
    struct directory* d = read_directory(base);
    char* rows[MAXPICK];
    int count;
    int i;
    int pick;
    if (d == NULL || d->count == 0) {
        if (d != NULL) delete_directory(d);
        free(base);
        gui_message("No Checkpoint backups were found.");
        return 0;
    }
    count = d->count < MAXPICK ? d->count : MAXPICK;
    for (i = 0; i < count; i++) {
        rows[i] = malloc(ROWN);
        relative_path(d->files[i], base, rows[i], ROWN);
        rows[i][ROWN - 1] = '\0';
    }
    pick = gui_pick_one("Select Checkpoint backup", rows, count);
    if (pick >= 0) strncpy(path, d->files[pick], size - 1);
    path[size - 1] = '\0';
    for (i = 0; i < count; i++) free(rows[i]);
    delete_directory(d);
    free(base);
    return pick >= 0;
}

int upload_flow(int type)
{
    int map = choose_mapping(type);
    int idx;
    char* id;
    char backup[PATHN];
    char names[MAXPICK * IDN];
    int remote_count;
    char* out = NULL;
    int out_size = 0;
    int status;
    char confirm[512];
    char remote_time[128];
    printf("Starting %s upload...\n", type ? "extdata" : "save");
    log_debug(type ? "starting extdata upload" : "starting save upload");
    if (map < 0) {
        printf("No mapping selected; returning to the Citrahold menu.\n");
        return 0;
    }
    idx = title_find(g_map_title[map]);
    if (idx < 0) return 0;
    log_debug("upload title selected");
    if (!choose_backup(idx, type, backup, PATHN)) return 0;
    log_debug("upload backup selected");
    remote_count = remote_games(type, names);
    if (remote_count < 0) {
        printf("Remote Game ID query failed before upload; continuing with unknown remote status.\n");
        log_debug("remote game list unavailable before upload");
    }
    else log_debug("upload remote game list received");
    remote_timestamp(type, g_map_game[map], remote_time, 128);
    log_debug("upload remote timestamp received");
    if (remote_count < 0) {
        sprintf(confirm, "Upload backup for Game ID:\n%s\n\nRemote copy: unavailable\nRemote time: unknown\n\nContinue?", g_map_game[map]);
    }
    else {
        sprintf(confirm, "Upload backup for Game ID:\n%s\n\nRemote copy: %s\nRemote time: %s\n\nContinue?", g_map_game[map], remote_has(names, remote_count, g_map_game[map]) ? "exists and will be replaced" : "not present", remote_time[0] == '\0' ? "unknown" : remote_time);
    }
    if (!gui_confirm(confirm)) return 0;
    log_debug("preparing upload payload");
    g_upload_error = 0;
    if (!write_upload_payload(backup, g_map_game[map])) {
        printf("Upload payload preparation failed.\n");
        if (g_upload_error == UPLOAD_ERR_READ) gui_message("A backup file or folder\ncould not be read.");
        else gui_message("Could not create the\ntemporary upload payload.");
        return 0;
    }
    log_debug("upload payload prepared");
    gui_status("Uploading Citrahold backup...");
    log_debug("sending upload request");
    status = api_upload_file(type ? "/uploadMultiExtdata" : "/uploadMultiSaves", g_upload_payload, &out, &out_size);
    unlink(g_upload_payload);
    if (out != NULL) free(out);
    printf("Upload response: HTTP %d\n", status);
    if (status != 200 && status != 201) {
        gui_message("Citrahold rejected the upload.");
        return 0;
    }
    if (g_delete_after && gui_confirm("Upload succeeded. Delete the local Checkpoint backup?")) {
        if (!remove_tree(backup)) gui_message("Upload succeeded, but the local backup could not be deleted.");
    }
    gui_message("Citrahold upload completed.");
    return 1;
}

int hex_or_base64_write(char* value, char* path)
{
    FILE* f;
    int len = strlen(value);
    int i = 0;
    int a;
    int b;
    int c;
    int d;
    char bytes[FILE_LIMIT];
    int out = 0;
    f = fopen(path, "wb");
    if (f == NULL) return 0;
    while (i < len) {
        if (i + 3 >= len) {
            fclose(f);
            return 0;
        }
        a = base64_value(value[i]);
        b = base64_value(value[i + 1]);
        c = value[i + 2] == '=' ? -1 : base64_value(value[i + 2]);
        d = value[i + 3] == '=' ? -1 : base64_value(value[i + 3]);
        if (a < 0 || b < 0 || (c < 0 && value[i + 2] != '=') || (d < 0 && value[i + 3] != '=') || (c < 0 && d >= 0) || ((c < 0 || d < 0) && i + 4 < len)) {
            fclose(f);
            return 0;
        }
        /* Flush before a quartet so every decoded three-byte group fits. */
        if (out > FILE_LIMIT - 3) {
            if (fwrite(bytes, 1, out, f) != out) {
                fclose(f);
                return 0;
            }
            out = 0;
        }
        bytes[out++] = (a << 2) | (b >> 4);
        if (c >= 0) bytes[out++] = ((b & 15) << 4) | (c >> 2);
        if (d >= 0) bytes[out++] = ((c & 3) << 6) | d;
        i = i + 4;
    }
    if (fwrite(bytes, 1, out, f) != out) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

int safe_remote_path(char* path)
{
    if (path == NULL || path[0] == '\0' || path[0] == '/' || path[0] == '\\') return 0;
    if (strstr(path, "..") != NULL || strchr(path, '\\') != NULL) return 0;
    return 1;
}

int range_total(char* headers)
{
    char* value;
    int i;
    int total = 0;
    if (headers == NULL) return 0;
    value = http_header_value(headers, "Content-Range");
    if (value == NULL) return 0;
    for (i = 0; value[i] != '\0'; i++) {
        if (value[i] == '/') {
            total = atoi(value + i + 1);
            break;
        }
    }
    free(value);
    return total;
}

int download_body(char* body, int body_size, char* game, char* file)
{
    body[0] = '\0';
    if (!append_text(body, "{\"token\":\"", body_size)) return 0;
    if (!append_json_string(body, active_token(), body_size)) return 0;
    if (!append_text(body, "\",\"game\":\"", body_size)) return 0;
    if (!append_json_string(body, game, body_size)) return 0;
    if (file != NULL) {
        if (!append_text(body, "\",\"file\":\"", body_size)) return 0;
        if (!append_json_string(body, file, body_size)) return 0;
    }
    return append_text(body, "\"}", body_size);
}

int write_download_chunk(char* path, char* data, int bytes, int append)
{
    FILE* f;
    int wrote;
    f = fopen(path, append ? "ab" : "wb");
    if (f == NULL) return 0;
    wrote = fwrite(data, 1, bytes, f);
    fclose(f);
    return wrote == bytes;
}

int download_remote_file(int type, char* game, char* remote, char* local, int file_number, int file_count)
{
    char body[PATHN * 2 + TOKENN + IDN + 64];
    char headers[96];
    char note[128];
    char* out = NULL;
    char* response_headers = NULL;
    int out_size = 0;
    int status;
    int received = 0;
    int total = 0;
    int first = 1;

    while (first || received < total) {
        sprintf(headers, "Range: bytes=%d-%d", received, received + DOWNLOAD_CHUNK - 1);
        if (!download_body(body, sizeof(body), game, remote)) return 0;
        out = NULL;
        response_headers = NULL;
        out_size = 0;
        sprintf(note, "Downloading file %d of %d", file_number, file_count);
        gui_status("Receiving Citrahold backup...");
        status = api_call_headers(type ? "/downloadExtdata" : "/downloadSaves", headers, body, &out, &out_size, &response_headers);
        if ((status != 206 && status != 200) || out == NULL || out_size <= 0 || out_size > DOWNLOAD_CHUNK) {
            if (out != NULL) free(out);
            if (response_headers != NULL) free(response_headers);
            printf("Download chunk failed: HTTP %d, %d bytes.\n", status, out_size);
            return 0;
        }
        if (first) {
            total = range_total(response_headers);
            if (total <= 0 && status == 200) total = out_size;
            if (total <= 0) {
                free(out);
                if (response_headers != NULL) free(response_headers);
                progress_end(1);
                return 0;
            }
            sprintf(note, "File %d of %d", file_number, file_count);
            progress_begin(1, note, total);
            first = 0;
        }
        if (received + out_size > total || !write_download_chunk(local, out, out_size, received > 0)) {
            free(out);
            if (response_headers != NULL) free(response_headers);
            progress_end(1);
            return 0;
        }
        received = received + out_size;
        progress_set(1, received);
        free(out);
        if (response_headers != NULL) free(response_headers);
        if (status == 200) break;
    }
    progress_end(1);
    return received == total;
}

int download_flow(int type)
{
    int map = choose_mapping(type);
    int idx;
    char* base;
    char backup_name[IDN];
    char temp[PATHN];
    char final_path[PATHN];
    char body[PATHN * 2 + TOKENN + IDN + 64];
    char* out = NULL;
    int out_size = 0;
    int status;
    struct JSON* root;
    struct JSON* files;
    int i;
    int count;
    char* key;
    char path[PATHN];
    char confirm[512];

    printf("Starting %s download...\n", type ? "extdata" : "save");
    log_debug(type ? "starting extdata download" : "starting save download");
    if (map < 0) {
        printf("No mapping selected; returning to the Citrahold menu.\n");
        return 0;
    }
    idx = title_find(g_map_title[map]);
    if (idx < 0) return 0;
    {
        char names[MAXPICK * IDN];
        int count_remote = remote_games(type, names);
        if (count_remote < 0 || !remote_has(names, count_remote, g_map_game[map])) {
            gui_message("That Game ID is not present on the server.");
            return 0;
        }
    }
    gui_keyboard(backup_name, "New Checkpoint backup name", IDN);
    if (backup_name[0] == '\0') return 0;
    base = title_backup_path(idx, type);
    sprintf(temp, "%s%s.citrahold-partial", base, backup_name);
    sprintf(final_path, "%s%s", base, backup_name);
    if (sd_exists(temp)) {
        if (!remove_tree(temp)) {
            free(base);
            gui_message("Could not remove the stale temporary download folder.");
            return 0;
        }
    }
    if (sd_exists(final_path)) {
        free(base);
        gui_message("That Checkpoint backup name already exists.");
        return 0;
    }
    sprintf(confirm, "Download Game ID:\n%s\ninto:\n%s\n\nContinue?", g_map_game[map], temp);
    if (!gui_confirm(confirm)) {
        free(base);
        return 0;
    }
    if (sd_mkdirs(temp) != 0) {
        free(base);
        gui_message("Could not create the temporary backup folder.");
        return 0;
    }

    if (!download_body(body, sizeof(body), g_map_game[map], NULL)) {
        remove_tree(temp);
        free(base);
        gui_message("Could not prepare the download request.");
        return 0;
    }
    gui_status("Receiving Citrahold backup...");
    progress_begin(0, "Listing Citrahold backup", 0);
    status = api_call(type ? "/downloadExtdata" : "/downloadSaves", body, &out, &out_size);
    progress_end(0);
    if (status != 200 || out == NULL || out_size > DOWNLOAD_RESPONSE_LIMIT - 1) {
        if (out != NULL) free(out);
        remove_tree(temp);
        free(base);
        printf("Download file list failed: HTTP %d, %d bytes.\n", status, out_size);
        gui_message("Citrahold download failed.");
        return 0;
    }
    root = json_new();
    json_parse(root, out);
    free(out);
    if (!json_object_contains(root, "files")) {
        json_delete(root);
        remove_tree(temp);
        free(base);
        gui_message("The server returned no files.");
        return 0;
    }
    files = json_object_element(root, "files");
    count = json_array_size(files);
    if (count <= 0) {
        json_delete(root);
        remove_tree(temp);
        free(base);
        gui_message("The remote Game ID contains no files.");
        return 0;
    }
    progress_begin(0, "Files", count);
    for (i = 0; i < count; i++) {
        key = json_get_string(json_array_element(files, i));
        if (!safe_remote_path(key) || strlen(key) >= PATHN - strlen(temp) - 2) {
            free(key);
            json_delete(root);
            remove_tree(temp);
            free(base);
            progress_end(0);
            gui_message("The server returned an unsafe file path.");
            return 0;
        }
        if (strcmp(key, "citraholdDirectoryDummy") == 0) {
            /* The server uses this marker for an otherwise empty root. */
        }
        else if (strstr(key, "citraholdDirectoryDummy") != NULL) {
            char* marker = strstr(key, "/citraholdDirectoryDummy");
            if (marker != NULL) *marker = '\0';
            sprintf(path, "%s/%s", temp, key);
            sd_mkdirs(path);
        }
        else {
            sprintf(path, "%s/%s", temp, key);
            {
                char parent[PATHN];
                parent_path(path, parent, PATHN);
                if (sd_mkdirs(parent) != 0 || !download_remote_file(type, g_map_game[map], key, path, i + 1, count)) {
                    free(key);
                    json_delete(root);
                    remove_tree(temp);
                    free(base);
                    progress_end(0);
                    gui_message("Citrahold download failed.");
                    return 0;
                }
            }
        }
        free(key);
        progress_set(0, i + 1);
    }
    progress_end(0);
    json_delete(root);
    if (rename(temp, final_path) != 0) {
        remove_tree(temp);
        free(base);
        gui_message("Download completed, but the backup could not be finalized.");
        return 0;
    }
    free(base);
    gui_message("Download completed and added to Checkpoint's backup list.");
    return 1;
}

void configuration_menu(void)
{
    char* options[8];
    int choice;
    options[0] = "Switch server mode";
    options[1] = "Authenticate active server";
    options[2] = "Test active server";
    options[3] = "Delete after upload";
    options[4] = "Debug logging";
    options[5] = "Set/change passphrase";
    options[6] = "Remove passphrase";
    options[7] = "Back";
    choice = gui_pick_one("Citrahold Configuration", options, 8);
    if (choice == 0) {
        if (choose_mode()) {
            if (state_write()) gui_message("Server mode saved.");
        }
    }
    else if (choice == 1) {
        char token[TOKENN];
        gui_keyboard(token, "Full Citrahold token", TOKENN);
        if (token[0] != '\0') {
            if (strcmp(g_mode, "official") == 0) strcpy(g_official_token, token);
            else strcpy(g_custom_token, token);
            state_write();
            verify_token();
        }
    }
    else if (choice == 2) server_test();
    else if (choice == 3) {
        g_delete_after = !g_delete_after;
        state_write();
        gui_message(g_delete_after ? "Delete-after-upload enabled." : "Delete-after-upload disabled.");
    }
    else if (choice == 4) {
        g_debug = !g_debug;
        state_write();
        gui_message(g_debug ? "Debug logging enabled." : "Debug logging disabled.");
    }
    else if (choice == 5) {
        gui_keyboard(g_pass, "Enter new passphrase", PASSN);
        if (g_pass[0] != '\0' && state_write()) gui_message("Passphrase saved.");
    }
    else if (choice == 6) {
        if (g_pass[0] == '\0') {
            gui_message("This state has no passphrase.");
        }
        else if (gui_confirm("Remove the passphrase? The state remains encrypted to this console.")) {
            g_pass[0] = '\0';
            state_write();
        }
    }
}

void upload_menu(void)
{
    char* options[3];
    int choice;
    options[0] = "Upload save backup";
    options[1] = "Upload extdata backup";
    options[2] = "Back";
    choice = gui_pick_one("Upload", options, 3);
    if (choice == 0) upload_flow(0);
    else if (choice == 1) upload_flow(1);
}

void download_menu(void)
{
    char* options[3];
    int choice;
    options[0] = "Download save backup";
    options[1] = "Download extdata backup";
    options[2] = "Back";
    choice = gui_pick_one("Download", options, 3);
    if (choice == 0) download_flow(0);
    else if (choice == 1) download_flow(1);
}

int main(int argc, char** argv)
{
    char* options[4];
    int choice;
    int loaded;
    init_paths();
    loaded = state_read();
    if (loaded == 0) {
        if (!configure_first_run()) return 0;
    }
    else if (loaded < 0) return 1;
    if (g_mode[0] == '\0') return 1;
    show_server_info();
    while (1) {
        printf("Citrahold menu ready. Hold B to exit.\n");
        options[0] = "Upload";
        options[1] = "Download";
        options[2] = "Manage Game IDs";
        options[3] = "Configuration";
        choice = gui_pick_one("Citrahold", options, 4);
        if (choice < 0) {
            printf("Citrahold script cancelled.\n");
            progress_clear();
            return 0;
        }
        if (choice == 0) upload_menu();
        else if (choice == 1) download_menu();
        else if (choice == 2) mappings_menu();
        else if (choice == 3) configuration_menu();
        progress_clear();
        printf("Action finished. Returning to the Citrahold menu.\n");
    }
}
