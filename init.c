#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <sys/utsname.h> // For uname() in ver command

#define CMD_BUF_SIZE 256
#define MAX_ARGS 32
#define PATH_MAX_LEN 1024
#define FILE_BUF_SIZE 4096

// --- Function Prototypes ---
void normalize_path_to_linux(char* path);
void format_path_for_dos(const char* linux_path, char* dos_path_buffer);
void show_help();
void show_version();
void show_about();
void copy_file(const char* source, const char* dest);
void do_dir(const char* path);
void do_xcopy(const char* source, const char* dest);

// --- Main Program Entry Point ---
int main() {
    char input_buf[CMD_BUF_SIZE];
    char* args[MAX_ARGS];
    char* token;

    // The kernel provides a working console. We just use it.
    printf("\nMyTinyDOS v0.0.1 - (c) 2025\n\n");

    while (1) {
        char real_cwd[PATH_MAX_LEN];
        char dos_prompt[PATH_MAX_LEN];

        if (getcwd(real_cwd, sizeof(real_cwd)) == NULL) {
            strcpy(real_cwd, "/");
        }
        format_path_for_dos(real_cwd, dos_prompt);
        printf("C:%s> ", dos_prompt);
        fflush(stdout);

        if (fgets(input_buf, sizeof(input_buf), stdin) == NULL) {
            continue;
        }
        
        input_buf[strcspn(input_buf, "\n")] = 0;
        if (strlen(input_buf) == 0) continue;

        int i = 0;
        token = strtok(input_buf, " ");
        while (token != NULL && i < MAX_ARGS - 1) {
            args[i++] = token;
            token = strtok(NULL, " ");
        }
        args[i] = NULL;
        if (args[0] == NULL) continue;
        char* command = args[0];

        for (int j = 1; args[j] != NULL; j++) {
            normalize_path_to_linux(args[j]);
        }

        // --- Built-in Command Dispatcher ---
        if (strcmp(command, "?") == 0 || strcmp(command, "help") == 0) {
            show_help();
        } else if (strcmp(command, "about") == 0) {
            show_about();
        } else if (strcmp(command, "ver") == 0) {
            show_version();
        } else if (strcmp(command, "cls") == 0) {
            printf("\033[2J\033[H");
        } else if (strcmp(command, "echo") == 0) {
            for (int j = 1; args[j] != NULL; j++) printf("%s ", args[j]);
            printf("\n");
        } else if (strcmp(command, "type") == 0) {
            if (args[1] == NULL) printf("Syntax: type [filename]\n");
            else {
                FILE *fp = fopen(args[1], "r");
                if (fp) {
                    char line[256];
                    while (fgets(line, sizeof(line), fp)) printf("%s", line);
                    fclose(fp);
                } else { perror("type"); }
            }
        } else if (strcmp(command, "copy") == 0) {
            if (args[1] == NULL || args[2] == NULL) printf("Syntax: copy [source] [destination]\n");
            else copy_file(args[1], args[2]);
        } else if (strcmp(command, "xcopy") == 0) {
            if (args[1] == NULL || args[2] == NULL) printf("Syntax: xcopy [source] [destination]\n");
            else do_xcopy(args[1], args[2]);
        } else if (strcmp(command, "del") == 0 || strcmp(command, "erase") == 0) {
            if (args[1] == NULL) printf("Syntax: del [filename]\n");
            else if (remove(args[1]) != 0) perror("del");
        } else if (strcmp(command, "ren") == 0 || strcmp(command, "rename") == 0 || strcmp(command, "move") == 0) {
            if (args[1] == NULL || args[2] == NULL) printf("Syntax: ren [old_name] [new_name]\n");
            else if (rename(args[1], args[2]) != 0) perror("ren");
        } else if (strcmp(command, "md") == 0 || strcmp(command, "mkdir") == 0) {
            if (args[1] == NULL) printf("Syntax: md [directory]\n");
            else if (mkdir(args[1], 0755) != 0) perror("md");
        } else if (strcmp(command, "rd") == 0 || strcmp(command, "rmdir") == 0) {
            if (args[1] == NULL) printf("Syntax: rd [directory]\n");
            else if (rmdir(args[1]) != 0) perror("rd");
        } else if (strcmp(command, "cd") == 0 || strcmp(command, "chdir") == 0) {
            if (args[1] == NULL) {
                printf("C:%s\n", dos_prompt);
            } else if (chdir(args[1]) != 0) {
                perror("cd");
            }
        } else if (strcmp(command, "dir") == 0) {
            do_dir(args[1] == NULL ? "." : args[1]);
        } else if (strcmp(command, "reboot") == 0) {
            printf("Rebooting system...\n");
            sync(); 
            reboot(RB_AUTOBOOT);
        } else if (strcmp(command, "exit") == 0 || strcmp(command, "shutdown") == 0) {
            printf("Shutting down system...\n");
            sync();
            reboot(RB_POWER_OFF);
        } else {
            pid_t pid = fork();
            if (pid == 0) {
                execvp(command, args);
                printf("Bad command or file name: '%s'\n", command);
                exit(1);
            } else if (pid > 0) {
                wait(NULL);
            } else {
                perror("fork");
            }
        }
    }
    return 0; // Should never be reached
}

// --- Helper Function Implementations ---

void normalize_path_to_linux(char* path) {
    if ((path[0] == 'C' || path[0] == 'c') && path[1] == ':') {
        memmove(path, path + 2, strlen(path));
    }
    for (int i = 0; path[i] != '\0'; i++) {
        if (path[i] == '\\') path[i] = '/';
    }
}

void format_path_for_dos(const char* linux_path, char* dos_path_buffer) {
    if(strcmp(linux_path, "/") == 0) {
        strcpy(dos_path_buffer, "\\");
        return;
    }
    strcpy(dos_path_buffer, linux_path);
    for (int i = 0; dos_path_buffer[i] != '\0'; i++) {
        if (dos_path_buffer[i] == '/') dos_path_buffer[i] = '\\';
    }
}

void show_help() {
    printf("\nMyTinyDOS v0.0.1 Command Reference\n");
    printf("  ? or HELP              Shows this help message.\n");
    printf("  ABOUT                  Shows author information.\n");
    printf("  VER                    Shows version information.\n");
    printf("  CLS                    Clears the screen.\n");
    printf("  ECHO [msg]             Displays a message.\n");
    printf("  DIR [path]             Lists directory contents.\n");
    printf("  CD [path]              Changes or shows the current directory.\n");
    printf("  MD/MKDIR [path]        Creates a directory.\n");
    printf("  RD/RMDIR [path]        Removes an empty directory.\n");
    printf("  TYPE [file]            Displays a file's content.\n");
    printf("  COPY [src] [dst]       Copies a single file.\n");
    printf("  XCOPY [src] [dst]      Copies files and directory trees.\n");
    printf("  DEL/ERASE [file]       Deletes a file.\n");
    printf("  REN/MOVE [src] [dst]   Renames or moves a file/directory.\n");
    printf("  REBOOT                 Restarts the system.\n");
    printf("  EXIT/SHUTDOWN          Powers off the system.\n\n");
}

void show_about() {
    printf("\nMyTinyDOS Shell\n");
    printf("  Author: minhmc2007\n");
    printf("  GitHub: https://github.com/minhmc2007\n\n");
}

void show_version() {
    struct utsname kernel_info;
    printf("MyTinyDOS Shell [Version 0.0.1 (2025)]\n");
    if (uname(&kernel_info) == 0) {
        printf("Running on Linux %s (%s)\n", kernel_info.release, kernel_info.machine);
    }
}

void copy_file(const char* source, const char* dest) {
    FILE *f_source, *f_dest;
    char buffer[FILE_BUF_SIZE];
    size_t bytes;
    f_source = fopen(source, "rb");
    if (!f_source) { perror("copy: source"); return; }
    f_dest = fopen(dest, "wb");
    if (!f_dest) { perror("copy: destination"); fclose(f_source); return; }
    while ((bytes = fread(buffer, 1, FILE_BUF_SIZE, f_source)) > 0) {
        if (fwrite(buffer, 1, bytes, f_dest) != bytes) {
            perror("copy: write error"); break;
        }
    }
    fclose(f_source); fclose(f_dest);
    printf("  1 file(s) copied.\n");
}

void do_xcopy(const char* source, const char* dest) {
    struct stat st;
    if (stat(source, &st) != 0) { perror("xcopy: source"); return; }
    if (S_ISDIR(st.st_mode)) {
        mkdir(dest, st.st_mode);
        DIR* dir = opendir(source);
        if (!dir) { perror("xcopy: opendir"); return; }
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            char new_source[PATH_MAX_LEN], new_dest[PATH_MAX_LEN];
            snprintf(new_source, sizeof(new_source), "%s/%s", source, entry->d_name);
            snprintf(new_dest, sizeof(new_dest), "%s/%s", dest, entry->d_name);
            do_xcopy(new_source, new_dest);
        }
        closedir(dir);
    } else {
        copy_file(source, dest);
    }
}

void do_dir(const char* path) {
    char dos_path_display[PATH_MAX_LEN];
    format_path_for_dos(path, dos_path_display);
    DIR* d = opendir(path);
    if (!d) { perror("dir"); return; }
    printf("\n Directory of C:%s\n\n", dos_path_display);
    struct dirent* dir;
    struct stat st;
    char full_path[PATH_MAX_LEN];
    char time_buf[80];
    long long total_size = 0;
    int file_count = 0;
    int dir_count = 0;
    while ((dir = readdir(d)) != NULL) {
        snprintf(full_path, sizeof(full_path), "%s/%s", path, dir->d_name);
        if (stat(full_path, &st) == -1) continue;
        strftime(time_buf, sizeof(time_buf), "%m/%d/%Y  %I:%M %p", localtime(&st.st_mtime));
        printf("%s   ", time_buf);
        if (S_ISDIR(st.st_mode)) {
            printf("%-12s ", "<DIR>");
            if (strcmp(dir->d_name, ".") != 0 && strcmp(dir->d_name, "..") != 0) dir_count++;
        } else {
            printf("%12lld ", (long long)st.st_size);
            total_size += st.st_size;
            file_count++;
        }
        printf("%s\n", dir->d_name);
    }
    closedir(d);
    printf("\n%15d File(s) %15lld bytes\n", file_count, total_size);
    printf("%15d Dir(s)\n", dir_count);
}
