static void history_save(void) {
    int pos = 0;
    int start = history_count > HISTORY_SIZE ? history_count - HISTORY_SIZE : 0;
    for (int i = start; i < history_count && pos < HISTORY_FILE_MAX - CMD_BUF_SIZE; i++) {
        const char *e = history[i % HISTORY_SIZE];
        int n = strlen(e);
        if (pos + n + 1 >= HISTORY_FILE_MAX) break;
        memcpy(history_filebuf + pos, e, n);
        pos += n;
        history_filebuf[pos++] = '\n';
    }
    history_filebuf[pos] = '\0';
    vfs_write(HISTORY_FILE, history_filebuf, (uint32_t)pos);
}

static void history_load(void) {
    uint32_t size = 0;
    if (vfs_read(HISTORY_FILE, history_filebuf, HISTORY_FILE_MAX - 1, &size) != 0) return;
    history_filebuf[size] = '\0';
    char *p = history_filebuf;
    while (*p) {
        char *nl = p;
        while (*nl && *nl != '\n') nl++;
        int len = (int)(nl - p);
        if (len > 0 && len < CMD_BUF_SIZE) {
            char tmp[CMD_BUF_SIZE];
            memcpy(tmp, p, len);
            tmp[len] = '\0';
            /* history_add without re-saving to avoid redundant writes */
            if (history_count == 0 ||
                strcmp(history[(history_count - 1) % HISTORY_SIZE], tmp) != 0) {
                strcpy(history[history_count % HISTORY_SIZE], tmp);
                history_count++;
            }
        }
        if (*nl == '\n') nl++;
        p = nl;
    }
}

static void history_add(const char *cmd) {
    if (cmd[0] == '\0') return;
    if (history_count > 0 && strcmp(history[(history_count - 1) % HISTORY_SIZE], cmd) == 0) return;
    strcpy(history[history_count % HISTORY_SIZE], cmd);
    history_count++;
    history_save();
}

void shell_history_add(const char *cmd_line) {
    history_add(cmd_line);
}

void shell_history_show_entries(void) {
    int start = history_count > HISTORY_SIZE ? history_count - HISTORY_SIZE : 0;
    for (int i = start; i < history_count; i++)
        kprintf("  %u: %s\n", (uint64_t)(i - start + 1), history[i % HISTORY_SIZE]);
}

int shell_history_count(void) {
    return history_count;
}

const char *shell_history_entry(int idx) {
    if (idx < 0 || idx >= history_count) return 0;
    int oldest = history_count > HISTORY_SIZE ? history_count - HISTORY_SIZE : 0;
    if (idx < oldest) return 0;
    return history[idx % HISTORY_SIZE];
}
