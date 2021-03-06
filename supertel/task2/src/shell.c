#include "shell.h"
#include "hash-functions.h"
#include "containers.h"
#include "common.h"
#include "protocol.h"
#include "log.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <dirent.h>

#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>

#define streq(a, b)         (0 == strcmp((a), (b)))
#define DELIM               " "
#define LIST_CMD            "list"
#define HELP_CMD            "help"
#define CMD_CMD             "cmd"

#define PROMPT              "> "
#define PROMPT_LEN          (sizeof(PROMPT) - 1)

#define HELP_MSG                                    \
"Commands:\n"                                       \
"list --- list all drivers\n"                       \
"help --- print this message\n"                     \
"cmd drv slot drv_cmd ... --- send command "        \
"drv_cmd to driver drv at slot with arguments\n"
#define HELP_MSG_LEN        (sizeof(HELP_MSG) - 1)

#define INVALID_MSG         "Invalid command\n"
#define INVALID_MSG_LEN     (sizeof(INVALID_MSG) - 1)

#define NEW_LINE            "\n"
#define NEW_LINE_LEN        (sizeof(NEW_LINE) - 1)

#define DRIVER_PRE          "Driver: "
#define DRIVER_PRE_LEN      (sizeof(DRIVER_PRE) - 1)

#define SLOT_PRE            "Slot: "
#define SLOT_PRE_LEN        (sizeof(SLOT_PRE) - 1)

#define DRIVER_POST         "Commands:\n"
#define DRIVER_POST_LEN     (sizeof(DRIVER_POST) - 1)

#define SPACE               " "
#define SPACE_LEN           (sizeof(SPACE) - 1)

#define DRIVER_SLOT_ID(name, name_len, slot)    \
char ID[name_len + MAX_DIGITS + 1];             \
size_t ID_len;                                  \
                                                \
ID_len = sprintf(ID, "%.*s%u",                  \
                 name_len, name, slot);

typedef struct {
    const char *driver_name;
    size_t driver_name_len;

    unsigned int slot_number;
} driver_description_t;

typedef struct {
    const char *arg;
    uint8_t len;
} arg_from_input_t;

typedef struct {
    uint8_t name[MAX_COMMAND_NAME_LEN + 1];
    uint8_t arity;
    uint8_t descr[MAX_COMMAND_DESCRIPTION_LEN + 1];
} shell_driver_command_t;

static
void purge_clients_list(avl_tree_node_t *atn);

static
void base_dir_event(int fd, io_svc_op_t op, shell_t *sh);

static inline
ssize_t base_dir_single_event(shell_t *sh, void *base, size_t offset);

static
void base_dir_smth_created(shell_t *sh, const char *name, size_t name_len);

static
void base_dir_smth_deleted(shell_t *sh, const char *name, size_t name_len);

static
void base_dir_self_deleted(shell_t *sh);

static inline
bool check_unix_socket(shell_t *sh, const char *name, size_t name_len);

static inline
bool parse_unix_socket_name(shell_t *sh, const char *name, size_t name_len,
                            driver_description_t *dd);

static inline
bool parse_unix_socket_name_(const char *name, size_t name_len,
                             driver_description_t *dd);

static
void connector(usc_t *usc, shell_t *sh);

static
void reader_signature(usc_t *usc, int error, shell_t *sh);

static
void reader_info(usc_t *usc, int error, shell_t *sh);
static
void reader_response(usc_t *usc, int error, shell_t *sh);

static
void on_input(int fd, io_svc_op_t op, shell_t *sh);

static inline
bool detect_newline_on_input(shell_t *sh);

static
void run_command_from_input(shell_t *sh);

static inline
void shift_input(shell_t *sh);

static
void cmd_list(shell_t *sh);

static
void cmd_help(shell_t *sh);

static
void cmd_invalid(shell_t *sh);

static
void cmd_cmd(shell_t *sh,
             const char *drv,
             unsigned int slot,
             const char *cmd,
             vector_t *args);

static inline
void finish_cmd(shell_t *sh);

static
void print_drv(shell_t *sh, avl_tree_node_t *atn);

static
void writer(usc_t *usc, int error, shell_t *sh);

static
void connect_to_all_existing_sockets(shell_t *sh);

static
int socket_name_filter(const struct dirent *de);

/********************** private **********************/
bool parse_unix_socket_name_(const char *name, size_t name_len, driver_description_t *dd) {
    /* path/name.slot.drv */
    size_t back = name_len - 1;
    size_t name_start, name_end;
    size_t slot_start, slot_end;

    while (back > 0 && name[back] != '/')
        -- back;

    for (name_start = name_end = back;
         name_end < name_len && name[name_end] != '.'; ++name_end);

    if (name_end == name_start || name_end == name_len)
        return false;

    for (slot_start = slot_end = name_end + 1;
         slot_end < name_len && name[slot_end] != '.' &&
         isdigit(name[slot_end]); ++slot_end);

    if (slot_end == slot_start || slot_end == name_len || name[slot_end] != '.')
        return false;

    if (name_len - slot_end != SUFFIX_LEN + 1)
        return false;

    if (strncmp(name + slot_end + 1, SUFFIX, SUFFIX_LEN))
        return false;

    dd->driver_name = name + name_start;
    dd->driver_name_len = name_end - name_start;
    dd->slot_number = atoi(name + slot_start);

    LOG(LOG_LEVEL_DEBUG, "Name parsed: %*s -> %*s (%u)\n",
        name_len, name, dd->driver_name_len, dd->driver_name,
        dd->slot_number);

    return true;
}

int socket_name_filter(const struct dirent *de) {
    bool ret;
    driver_description_t dd;

    if (DT_SOCK != de->d_type)
        return 0;

    return parse_unix_socket_name_(de->d_name, strlen(de->d_name), &dd);
}

void connect_to_all_existing_sockets(shell_t *sh) {
    struct dirent **entries = NULL;
    int number, idx;

    number = scandir(".", &entries,
                     socket_name_filter, alphasort);

    if (number < 0) {
        LOG(LOG_LEVEL_FATAL, "Can't scan current directory: %s\n",
            strerror(errno));
        abort();
    }

    for (idx = 0; idx < number; ++idx) {
        struct dirent *e = entries[idx];

        base_dir_smth_created(sh, e->d_name, strlen(e->d_name));
    }

    free(entries);
}

bool check_unix_socket(shell_t *sh, const char *name, size_t name_len) {
    int rc;
    struct stat st;

    rc = stat(name, &st);

    if (rc) {
        LOG(LOG_LEVEL_WARN, "Can't stat %*s: %s\n",
            name_len, name, strerror(errno));
        return false;
    }

    return S_ISSOCK(st.st_mode);
}

bool parse_unix_socket_name(shell_t *sh,
                            const char *name, size_t name_len,
                            driver_description_t *dd) {
    return parse_unix_socket_name_(name, name_len, dd);
}

void print_drv(shell_t *sh, avl_tree_node_t *atn) {
    shell_driver_t *sd;
    list_t *l;
    list_element_t *le;
    size_t idx;

    if (!atn)
        return;

    print_drv(sh, atn->left);
    print_drv(sh, atn->right);

    l = (list_t *)atn->data;

    for (le = list_begin(l); le; le = list_next(l, le)) {
        sd = (shell_driver_t *)le->data;

        fprintf(
            sh->output,
            NEW_LINE
            DRIVER_PRE "%.*s" NEW_LINE
            SLOT_PRE "%u" NEW_LINE,
            sd->name_len, sd->name,
            sd->slot
        );

        for (idx = 0; idx < vector_count(&sd->commands); ++idx) {
            const shell_driver_command_t *sdc =
                (const shell_driver_command_t *)vector_get(&sd->commands, idx);

            fprintf(
                sh->output,
                "%s <arity: %u> --- %s" NEW_LINE,
                sdc->name,
                (unsigned int)sdc->arity,
                sdc->descr
            );
        }
    }
}

void cmd_list(shell_t *sh) {
    print_drv(sh, sh->clients.root);
    finish_cmd(sh);
}

void cmd_help(shell_t *sh) {
    fprintf(sh->output, HELP_MSG);
    finish_cmd(sh);
}

void cmd_invalid(shell_t *sh) {
    fprintf(sh->output, INVALID_MSG);
    finish_cmd(sh);
}

void cmd_cmd(shell_t *sh,
             const char *drv, unsigned int slot, const char *cmd,
             vector_t *args) {
    buffer_t b;
    size_t length_required;
    const shell_driver_command_t *sdc;
    pr_driver_command_argument_t *pdca;
    pr_driver_command_t *pdc;
    size_t idx;
    arg_from_input_t *afi;
    uint8_t *arg_val;
    avl_tree_node_t *atn;
    hash_t hash;
    list_t *l;
    list_element_t *le;
    shell_driver_t *sd;
    size_t cmd_arity;
    size_t cmd_idx;
    size_t drv_len = strlen(drv);

    DRIVER_SLOT_ID(drv, drv_len, slot);
    hash = hash_pearson(ID, ID_len);
    atn = avl_tree_get(&sh->clients, hash);
    if (!atn) {
        LOG(LOG_LEVEL_WARN, "Can't find driver %s at slot %u (1)\n",
            drv, slot);
        cmd_invalid(sh);
        return;
    }

    l = (list_t *)atn->data;

    for (le = list_begin(l); le; le = list_next(l, le)) {
        sd = (shell_driver_t *)le->data;

        if (sd->name_len != drv_len)
            continue;

        if ((0 == strncmp(sd->name, drv, drv_len)) &&
            sd->slot == slot)
            break;
    }

    if (!le) {
        LOG(LOG_LEVEL_WARN, "Can't find driver %s at slot %u (2)\n",
            drv, slot);
        cmd_invalid(sh);
        return;
    }

    /* find command at fetch its index */

    for (idx = 0; idx < vector_count(&sd->commands); ++idx) {
        sdc = (const shell_driver_command_t *)vector_get(&sd->commands, idx);

        if (0 == strncmp((const char *)sdc->name, cmd, MAX_COMMAND_NAME_LEN)) {
            cmd_arity = sdc->arity;
            break;
        }
    }

    if (vector_count(&sd->commands) == idx) {
        LOG(LOG_LEVEL_WARN,
            "Couldn't find command %s for driver %s at slot %u\n",
            cmd, drv, slot);
        cmd_invalid(sh);
        return;
    }

    cmd_idx = idx;

    if (cmd_arity < vector_count(args)) {
        LOG_MSG(LOG_LEVEL_WARN, "Too many arguments for the command supplied\n");
        cmd_invalid(sh);
        return;
    }

    length_required = sizeof(*pdc) + vector_count(args) * sizeof(*pdca);

    for (idx = 0; idx < vector_count(args); ++idx) {
        afi = (arg_from_input_t *)vector_get(args, idx);
        length_required += afi->len;
    }

    buffer_init(&b, length_required, bp_non_shrinkable);

    pdc = (pr_driver_command_t *)b.data;
    pdc->s.s = PR_DRV_COMMAND;
    pdc->cmd_idx = cmd_idx;
    pdc->argc = vector_count(args);
    pdca = (pr_driver_command_argument_t *)(pdc + 1);

    for (idx = 0; idx < vector_count(args); ++idx) {
        afi = (arg_from_input_t *)vector_get(args, idx);

        pdca->len = afi->len;
        arg_val = (uint8_t *)(pdca + 1);
        memcpy(arg_val, afi->arg, pdca->len);

        pdca = (pr_driver_command_argument_t *)(arg_val + pdca->len);
    }

    unix_socket_client_send(
        &sd->usc, b.data, b.user_size,
        (usc_writer_t)writer, sh
    );

    buffer_deinit(&b);
}

void finish_cmd(shell_t *sh) {
    fprintf(sh->output, PROMPT);
}

bool detect_newline_on_input(shell_t *sh) {
    if (sh->input_buffer.user_size == 0)
        return false;

    while (sh->input_buffer.offset < sh->input_buffer.user_size) {
        if ('\n' == ((char *)sh->input_buffer.data)[sh->input_buffer.offset])
            return true;

        ++sh->input_buffer.offset;
    }

    return false;
}

void run_command_from_input(shell_t *sh) {
    char *line = (char *)sh->input_buffer.data;
    char *token;
    char *cmd, *drv, *slot, *drv_cmd;
    char *slot_endptr = NULL;
    unsigned int slot_number;
    vector_t args;
    arg_from_input_t *afi;
    size_t arg_len;

    line[sh->input_buffer.offset] = '\0';

    cmd = strtok(line, DELIM);

    if (!cmd) {
        LOG_MSG(LOG_LEVEL_WARN, "Invalid input: no command\n");
        cmd_invalid(sh);
        return;
    }

    if (streq(LIST_CMD, cmd)) {
        cmd_list(sh);
        return;
    }
    else if (streq(HELP_CMD, cmd)) {
        cmd_help(sh);
        return;
    }
    else if (!streq(CMD_CMD, cmd)) {
        cmd_invalid(sh);
        return;
    }

    drv = strtok(NULL, DELIM);
    slot = strtok(NULL, DELIM);
    drv_cmd = strtok(NULL, DELIM);

    if (!drv || !slot || !drv_cmd) {
        LOG(LOG_LEVEL_WARN, "Invalid input: %s %s %s\n",
            drv ? drv : "n / a",
            slot ? slot : "n / a",
            drv_cmd ? drv_cmd : "n / a");
        cmd_invalid(sh);
        return;
    }

    slot_number = strtoul(slot, &slot_endptr, 10);
    if (*slot_endptr != '\0') {
        LOG_MSG(LOG_LEVEL_WARN, "Slot is not number\n");
        cmd_invalid(sh);
        return;
    }

    vector_init(&args, sizeof(*afi), 0);
    token = strtok(NULL, DELIM);

    while (token) {
        arg_len = strlen(token);

        if (arg_len > 255) {
            LOG(LOG_LEVEL_WARN, "Too long argument: %s\n",
                token);
            vector_deinit(&args);
            cmd_invalid(sh);
            return;
        }

        afi = (arg_from_input_t *)vector_append(&args);
        afi->arg = token;
        afi->len = arg_len;

        token = strtok(NULL, DELIM);
    }

    cmd_cmd(sh, drv, slot_number, drv_cmd, &args);

    vector_deinit(&args);

    return;
}

void shift_input(shell_t *sh) {
    uint8_t *start = (uint8_t *)sh->input_buffer.data;

    memmove(start, start + sh->input_buffer.offset + 1,
            sh->input_buffer.user_size - sh->input_buffer.offset - 1);

    buffer_realloc(&sh->input_buffer,
                   sh->input_buffer.user_size - sh->input_buffer.offset - 1);
}

void on_input(int fd, io_svc_op_t op, shell_t *sh) {
    int pending;
    int rc;
    size_t old_size = sh->input_buffer.user_size;
    ssize_t current_read;
    size_t bytes_read;

    rc = ioctl(fd, FIONREAD, &pending);

    if (rc < 0) {
        LOG(LOG_LEVEL_FATAL, "Can't ioctl(FIONREAD) on input FD: %s\n",
            strerror(errno));
        abort();
    }

    if (0 == pending) /* EOF */ {
        io_service_stop(sh->iosvc, false);
        return;
    }

    buffer_realloc(&sh->input_buffer, sh->input_buffer.user_size + pending);

    bytes_read = 0;
    while (pending) {
        current_read = read(fd, sh->input_buffer.data + old_size + bytes_read,
                            pending);
        if (current_read < 0) {
            if (errno == EINTR)
                continue;
            else {
                LOG(LOG_LEVEL_FATAL, "Can't read input: %s\n",
                    strerror(errno));
                abort();
            }
        }

        bytes_read += current_read;
        pending -= current_read;
    }

    while (detect_newline_on_input(sh)) {
        run_command_from_input(sh);
        shift_input(sh);
        sh->input_buffer.offset = 0;
    }
}

void writer(usc_t *usc, int error, shell_t *sh) {
    buffer_realloc(&usc->read_task.b, 0);
    buffer_realloc(&usc->write_task.b, 0);

    if (error) {
        LOG(LOG_LEVEL_WARN, "Couldn't send to driver %*s: %s\n",
            usc->connected_to_name_len, usc->connected_to_name,
            strerror(errno));

        if (!unix_socket_client_reconnect(usc)) {
            LOG(LOG_LEVEL_WARN, "Couldn't reconnect to driver %*s: %s\n",
                usc->connected_to_name_len, usc->connected_to_name,
                strerror(errno));
            return;
        }

        LOG_MSG(LOG_LEVEL_WARN, "Repeat your command\n");
        fprintf(sh->output, PROMPT);
        return;
    }

    unix_socket_client_recv(usc, sizeof(pr_signature_t),
                            (usc_reader_t)reader_signature, sh);
}

#define READ_ERROR_HANDLER                                                      \
do {                                                                            \
    if (error) {                                                                \
        LOG(LOG_LEVEL_WARN, "Error on receive: %s\n",                           \
            strerror(errno));                                                   \
        LOG_MSG(LOG_LEVEL_WARN, "Reconnecting\n");                              \
                                                                                \
        if (!unix_socket_client_reconnect(usc))                                 \
            LOG(LOG_LEVEL_FATAL, "Can't reconnect to %*s: %s\n",                \
                usc->connected_to_name_len, usc->connected_to_name,             \
                strerror(errno));                                               \
                                                                                \
        return;                                                                 \
    }                                                                           \
                                                                                \
    if (usc->eof) {                                                             \
        LOG(LOG_LEVEL_WARN, "EOF from %*s.\n",                                  \
            usc->connected_to_name_len, usc->connected_to_name);                \
        LOG_MSG(LOG_LEVEL_WARN, "Possibly a delete will take place soon\n");    \
        return;                                                                 \
    }                                                                           \
} while(0)

void reader_info(usc_t *usc, int error, shell_t *sh) {
    size_t required_length;
    size_t idx;
    shell_driver_t *sd;
    shell_driver_command_t *sdc;
    const pr_driver_command_info_t *dci;
    const pr_driver_info_t *di = (const pr_driver_info_t *)usc->read_task.b.data;

    READ_ERROR_HANDLER;

    required_length = sizeof(*di) + sizeof(*dci) * di->commands_number;

    if (usc->read_task.b.user_size < required_length) {
        usc->read_task.b.offset = usc->read_task.b.user_size;
        unix_socket_client_recv(
            usc,
            required_length - usc->read_task.b.user_size,
            (usc_reader_t)reader_info,
            sh);

        return;
    }

    sd = (shell_driver_t *)usc->priv;
    dci = (const pr_driver_command_info_t *)(di + 1);
    vector_init(&sd->commands, sizeof(*sdc), di->commands_number);
    for (idx = 0; idx < di->commands_number; ++idx, ++dci) {
        sdc = (shell_driver_command_t *)vector_get(&sd->commands, idx);

        memcpy(sdc->name, dci->name, MAX_COMMAND_NAME_LEN);
        sdc->name[MAX_COMMAND_NAME_LEN] = '\0';
        memcpy(sdc->descr, dci->descr, MAX_COMMAND_DESCRIPTION_LEN);
        sdc->descr[MAX_COMMAND_DESCRIPTION_LEN] = '\0';
        sdc->arity = dci->arity;
    }
}

void reader_response(usc_t *usc, int error, shell_t *sh) {
    size_t required_length;
    const pr_driver_response_t *dr = (const pr_driver_response_t *)usc->read_task.b.data;

    READ_ERROR_HANDLER;

    required_length = sizeof(*dr) + dr->len;

    if (usc->read_task.b.user_size < required_length) {
        usc->read_task.b.offset = usc->read_task.b.user_size;
        unix_socket_client_recv(
            usc,
            required_length - usc->read_task.b.user_size,
            (usc_reader_t)reader_response,
            sh);

        return;
    }

    fprintf(sh->output, "%.*s" NEW_LINE, dr->len, (const char *)(dr + 1));
    finish_cmd(sh);
}

void reader_signature(usc_t *usc, int error, shell_t *sh) {
    const pr_signature_t *s = (const pr_signature_t *)usc->read_task.b.data;

    READ_ERROR_HANDLER;

    usc->read_task.b.offset = sizeof(*s);

    switch (s->s) {
        case PR_DRV_INFO:
            unix_socket_client_recv(usc,
                                    sizeof(pr_driver_info_t) - sizeof(*s),
                                    (usc_reader_t)reader_info, sh);
            break;
        case PR_DRV_RESPONSE:
            unix_socket_client_recv(usc,
                                    sizeof(pr_driver_response_t) - sizeof(*s),
                                    (usc_reader_t)reader_response, sh);
            break;
        case PR_DRV_COMMAND:
        default:
            LOG(LOG_LEVEL_WARN, "Invalid signature %#02x from %*s\n",
                (unsigned int)s->s,
                usc->connected_to_name_len,
                usc->connected_to_name);
            LOG_MSG(LOG_LEVEL_WARN, "Reconnecting\n");

            if (!unix_socket_client_reconnect(usc))
                LOG(LOG_LEVEL_FATAL, "Can't reconnect to %*s: %s\n",
                    usc->connected_to_name_len, usc->connected_to_name,
                    strerror(errno));

            break;
    }
}
#undef READ_ERROR_HANDLER

void connector(usc_t *usc, shell_t *sh) {
    buffer_realloc(&usc->read_task.b, 0);
    buffer_realloc(&usc->write_task.b, 0);

    unix_socket_client_recv(usc, sizeof(pr_signature_t),
                            (usc_reader_t)reader_signature, sh);
}

static
void purge_clients_list(avl_tree_node_t *atn) {
    list_t *l;
    list_element_t *le;
    shell_driver_t *sd;

    if (!atn)
        return;

    purge_clients_list(atn->left);
    purge_clients_list(atn->right);

    l = (list_t *)atn->data;

    for (le = list_begin(l); le; le = list_next(l, le)) {
        sd = (shell_driver_t *)le->data;

        unix_socket_client_deinit(&sd->usc);
        free(sd->name);
    }

    list_purge(l);
}

void base_dir_self_deleted(shell_t *sh) {
    LOG_MSG(LOG_LEVEL_WARN, "Base directory deleted.\n");
    LOG_MSG(LOG_LEVEL_WARN, "  Stopping. Won't wait for pending events.\n");

    sh->running = false;
    io_service_stop(sh->iosvc, false);
}

void base_dir_smth_created(shell_t *sh, const char *name, size_t name_len) {
    driver_description_t dd;
    hash_t hash;
    avl_tree_node_t *atn;
    list_t *l;
    list_element_t *le;
    shell_driver_t *sd;
    bool inserted = false;

    LOG(LOG_LEVEL_DEBUG, "Created: %*s\n",
        name_len, name);

    if (!check_unix_socket(sh, name, name_len)) {
        LOG(LOG_LEVEL_DEBUG, "It is not a readable UNIX socket: %*s\n",
            name_len, name);
        return;
    }

    if (!parse_unix_socket_name(sh, name, name_len, &dd)) {
        LOG(LOG_LEVEL_DEBUG, "It is not valid readable UNIX socket name: %*s\n",
            name_len, name);
        return;
    }

    DRIVER_SLOT_ID(dd.driver_name, dd.driver_name_len, dd.slot_number);

    hash = hash_pearson(ID, ID_len);

    atn = avl_tree_add_or_get(&sh->clients, hash, &inserted);

    l = (list_t *)atn->data;

    if (inserted)
        list_init(l, true, sizeof(*sd));

    for (le = list_begin(l); le; le = list_next(l, le)) {
        sd = (shell_driver_t *)le->data;

        if (sd->name_len != dd.driver_name_len)
            continue;

        if ((0 == strncmp(sd->name, dd.driver_name, name_len)) &&
            sd->slot == dd.slot_number)
            break;
    }

    if (le) {
        LOG(LOG_LEVEL_FATAL, "Duplicate driver: %*s\n",
            name_len, name);
        abort();
    }

    le = list_append(l);
    sd = (shell_driver_t *)le->data;

    if (!unix_socket_client_init(&sd->usc, NULL, 0, sh->iosvc)) {
        LOG(LOG_LEVEL_FATAL,
            "Can't initialize UNIX socket client for %*s: %s\n",
            name_len, name, strerror(errno));
        abort();
    }

    sd->usc.priv = sd;

    sd->name_len = dd.driver_name_len;
    sd->name = (char *)malloc(dd.driver_name_len + 1);
    sd->slot = dd.slot_number;

    memcpy(sd->name, dd.driver_name, dd.driver_name_len);
    sd->name[dd.driver_name_len] = '\0';

    if (!unix_socket_client_connect(
        &sd->usc, name, name_len,
        (usc_connector_t)connector, sh))
        LOG(LOG_LEVEL_FATAL, "Can't connect to %*s: %s\n",
            name_len, name, strerror(errno));
}

void base_dir_smth_deleted(shell_t *sh, const char *name, size_t name_len) {
    driver_description_t dd;
    hash_t hash;
    avl_tree_node_t *atn;
    list_t *l;
    list_element_t *le;
    shell_driver_t *sd;

    LOG(LOG_LEVEL_DEBUG, "Deleted: %*s\n",
        name_len, name);

    if (!parse_unix_socket_name(sh, name, name_len, &dd)) {
        LOG(LOG_LEVEL_DEBUG, "It is not valid readable UNIX socket name: %*s\n",
            name_len, name);
        return;
    }

    DRIVER_SLOT_ID(dd.driver_name, dd.driver_name_len, dd.slot_number);

    hash = hash_pearson(ID, ID_len);

    atn = avl_tree_get(&sh->clients, hash);

    if (!atn) {
        LOG(LOG_LEVEL_WARN, "UNIX socket name was not registered (1): %*s\n",
            name_len, name);
        return;
    }

    l = (list_t *)atn->data;

    for (le = list_begin(l); le; le = list_next(l, le)) {
        sd = (shell_driver_t *)le->data;

        if (sd->name_len != dd.driver_name_len)
            continue;

        if ((0 == strncmp(sd->name, dd.driver_name, sd->name_len)) &&
            sd->slot == dd.slot_number)
            break;
    }

    if (!le) {
        LOG(LOG_LEVEL_WARN, "UNIX socket name was not registered (2): %*s\n",
            name_len, name);
        return;
    }

    unix_socket_client_deinit(&sd->usc);
    free(sd->name);

    list_remove_and_advance(l, le);
}

ssize_t base_dir_single_event(shell_t *sh, void *base, size_t _offset) {
    ssize_t offset = 0;
    struct inotify_event *event = base + _offset;

    offset = sizeof(*event);

    if (event->len)
        offset += event->len;

    if (event->len)
        event->len = strlen((const char *)event->name);

    if (event->mask & IN_CREATE)
        base_dir_smth_created(sh, event->name, event->len);

    if (event->mask & IN_DELETE)
        base_dir_smth_deleted(sh, event->name, event->len);

    if (event->mask & IN_DELETE_SELF)
        base_dir_self_deleted(sh);

    return offset;
}

void base_dir_event(int fd, io_svc_op_t op, shell_t *sh) {
    int pending;
    int rc;
    buffer_t b;
    ssize_t data_read;

    LOG_MSG(LOG_LEVEL_DEBUG, "Events for base dir\n");

    rc = ioctl(fd, FIONREAD, &pending);

    if (rc < 0) {
        LOG(LOG_LEVEL_FATAL, "Can't ioctl (FIONREAD) inotify instance: %s\n",
            strerror(errno));
        abort();
    }

    buffer_init(&b, pending, bp_non_shrinkable);

    b.offset = 0;

    while (b.offset < b.user_size) {
        data_read = read(fd, b.data + b.offset,
                         b.user_size - b.offset);

        if (data_read < 0) {
            if (errno == EINTR)
                continue;
            else
                break;
        }

        b.offset += data_read;
    }

    if (b.offset < b.user_size) {
        LOG(LOG_LEVEL_FATAL, "Couldn't read inotify events descriptions: %s\n",
            strerror(errno));
        abort();
    }

    b.offset = 0;

    while ((b.offset < b.user_size) && sh->running) {
        data_read = base_dir_single_event(sh, b.data, b.offset);

        if (data_read < 0) {
            LOG_MSG(LOG_LEVEL_FATAL,
                    "Couldn't read and react for single inotify event\n");
            abort();
        }

        b.offset += data_read;
    }

    buffer_deinit(&b);
}

/********************** API **********************/
bool shell_init(shell_t *sh,
                const char *base_path, size_t base_path_len,
                io_service_t *iosvc, int input_fd, FILE *output) {
    bool ret;

    assert(sh && iosvc && (!base_path_len || (base_path && base_path_len)));

    memset(sh, 0, sizeof(*sh));

    sh->iosvc = iosvc;

    sh->inotify_fd = inotify_init();

    if (sh->inotify_fd < 0)
        return false;

    sh->base_path_watch_descriptor = -1;

    avl_tree_init(&sh->clients, true, sizeof(list_t));

    if (0 == base_path_len) {
        base_path = DOT;
        base_path_len = DOT_LEN;
    }

    sh->base_path = (char *)malloc(base_path_len + 1);
    memcpy(sh->base_path, base_path, base_path_len);
    sh->base_path_len = base_path_len -
                            ('/' == sh->base_path[base_path_len - 1]);
    sh->base_path[sh->base_path_len] = '\0';

    sh->running = false;

    buffer_init(&sh->input_buffer, 0, bp_non_shrinkable);

    sh->input_fd = input_fd;
    sh->output = output;

    return true;
}

void shell_deinit(shell_t *sh) {
    assert(sh);

    if (!(sh->base_path_watch_descriptor < 0))
        inotify_rm_watch(sh->inotify_fd, sh->base_path_watch_descriptor);

    close(sh->inotify_fd);

    purge_clients_list(sh->clients.root);

    free(sh->base_path);

    buffer_deinit(&sh->input_buffer);
}

void shell_run(shell_t *sh) {
    int wd;

    assert(sh);

    mkdir(sh->base_path, S_IRUSR | S_IWUSR | S_IXUSR);

    if (chdir(sh->base_path)) {
        LOG(LOG_LEVEL_WARN, "Can't chdir to %s: %s\n",
            sh->base_path, strerror(errno));
        abort();
    }

    wd = inotify_add_watch(sh->inotify_fd, ".",
                           IN_CREATE | IN_DELETE | IN_DELETE_SELF |
                            IN_EXCL_UNLINK | IN_ONLYDIR);

    if (wd < 0) {
        LOG(LOG_LEVEL_FATAL, "Couldn't allocate watch descriptor for %s: %s\n",
            sh->base_path, strerror(errno));

        return;
    }

    sh->base_path_watch_descriptor = wd;

    sh->running = true;

    connect_to_all_existing_sockets(sh);

    fprintf(sh->output, PROMPT);

    io_service_post_job(sh->iosvc, sh->inotify_fd,
                        IO_SVC_OP_READ, !IOSVC_JOB_ONESHOT,
                        (iosvc_job_function_t)base_dir_event,
                        sh);

    io_service_post_job(sh->iosvc, sh->input_fd,
                        IO_SVC_OP_READ, !IOSVC_JOB_ONESHOT,
                        (iosvc_job_function_t)on_input,
                        sh);

    io_service_run(sh->iosvc);
}
