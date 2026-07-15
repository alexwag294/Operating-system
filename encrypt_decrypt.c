#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ─────────────────────────────────────────────────
   ANSI COLOURS
───────────────────────────────────────────────── */
#define RESET   "\033[0m"
#define BOLD    "\033[1m"
#define DIM     "\033[2m"
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define BLUE    "\033[34m"
#define CYAN    "\033[36m"
#define CLEAR   "\033[2J\033[H"

/* ─────────────────────────────────────────────────
   CONSTANTS
───────────────────────────────────────────────── */
#define MAX_USERS    5
#define MAX_NAME     32
#define MAX_PASS     64
#define MAX_INPUT    512
#define AUDIT_LOG    "audit.log"
#define USER_DB      "users.db"
#define FILES_DIR    "fs_files"
#define XOR_KEY      0x5A

/* ─────────────────────────────────────────────────
   PERMISSION BITS  (Unix-style octal)
───────────────────────────────────────────────── */
#define PERM_OWNER_R  0400
#define PERM_OWNER_W  0200
#define PERM_OWNER_X  0100
#define PERM_OTHER_R  0004
#define PERM_OTHER_W  0002

/* ─────────────────────────────────────────────────
   STRUCTS
───────────────────────────────────────────────── */
typedef struct {
    char username[MAX_NAME];
    char password_hash[MAX_PASS];
    int  is_owner;
} User;

/* ─────────────────────────────────────────────────
   SESSION
───────────────────────────────────────────────── */
User current_user;
int  logged_in = 0;

/* ─────────────────────────────────────────────────
   HELPERS
───────────────────────────────────────────────── */

/* Read a line from stdin, strip newline */
void input(const char *prompt, char *buf, int size) {
    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buf, size, stdin))
        buf[strcspn(buf, "\n")] = '\0';
}

/* Read password without echo */
void input_password(const char *prompt, char *buf, int size) {
    printf("%s", prompt);
    fflush(stdout);
    system("stty -echo");
    if (fgets(buf, size, stdin))
        buf[strcspn(buf, "\n")] = '\0';
    system("stty echo");
    printf("\n");
}

void divider(void) {
    printf("============================================================\n");
}

void header(const char *title) {
    printf("\n");
    divider();
    printf("  %s\n", title);
    divider();
    printf("\n");
}

/* Permission guide — shown wherever user picks permissions */
void print_perm_guide(void) {
    printf("  Permission Guide:\n");
    printf("    600  =  only you can read and write             (rw-------)\n");
    printf("    640  =  you can read/write, group can read      (rw-r-----)\n");
    printf("    644  =  you can read/write, everyone can read   (rw-r--r--)\n");
    printf("    700  =  only you can read, write and execute    (rwx------)\n");
    printf("    777  =  everyone can read, write and execute    (rwxrwxrwx)\n");
    printf("\n");
}

/* ─────────────────────────────────────────────────
   PASSWORD HASH
   XOR each byte with XOR_KEY, store as hex string.
   Passwords are never saved in plaintext.
───────────────────────────────────────────────── */
void hash_password(const char *pw, char *out) {
    int len = strlen(pw);
    for (int i = 0; i < len; i++)
        sprintf(out + i * 2, "%02x", (unsigned char)(pw[i] ^ XOR_KEY));
    out[len * 2] = '\0';
}

/* ─────────────────────────────────────────────────
   AUDIT LOG
   Every action is recorded with timestamp, user,
   action type, and filename.
───────────────────────────────────────────────── */
void audit(const char *action, const char *file) {
    FILE *f = fopen(AUDIT_LOG, "a");
    if (!f) return;
    time_t now = time(NULL);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(f, "[%s] USER=%-14s ACTION=%-14s FILE=%s\n",
            ts,
            logged_in ? current_user.username : "anonymous",
            action,
            file ? file : "-");
    fclose(f);
}

/* ─────────────────────────────────────────────────
   USER DB HELPERS
───────────────────────────────────────────────── */
int load_users(User *users, int *count) {
    FILE *f = fopen(USER_DB, "r");
    if (!f) { *count = 0; return 0; }
    *count = 0;
    while (*count < MAX_USERS &&
           fscanf(f, "%31s %63s %d",
                  users[*count].username,
                  users[*count].password_hash,
                  &users[*count].is_owner) == 3)
        (*count)++;
    fclose(f);
    return 1;
}

void save_users(User *users, int count) {
    FILE *f = fopen(USER_DB, "w");
    if (!f) return;
    for (int i = 0; i < count; i++)
        fprintf(f, "%s %s %d\n",
                users[i].username,
                users[i].password_hash,
                users[i].is_owner);
    fclose(f);
}

/* ─────────────────────────────────────────────────
   PERMISSION HELPERS
───────────────────────────────────────────────── */
void meta_path(const char *filename, char *out) {
    snprintf(out, 256, "%s/.%s.meta", FILES_DIR, filename);
}

void write_meta(const char *filename, const char *owner, int perm) {
    char mp[256]; meta_path(filename, mp);
    FILE *f = fopen(mp, "w");
    if (!f) return;
    fprintf(f, "%s %o\n", owner, perm);
    fclose(f);
}

int read_meta(const char *filename, char *owner, int *perm) {
    char mp[256]; meta_path(filename, mp);
    FILE *f = fopen(mp, "r");
    if (!f) return 0;
    fscanf(f, "%31s %o", owner, perm);
    fclose(f);
    return 1;
}

void perm_to_str(int perm, char *out) {
    out[0] = (perm & PERM_OWNER_R) ? 'r' : '-';
    out[1] = (perm & PERM_OWNER_W) ? 'w' : '-';
    out[2] = (perm & PERM_OWNER_X) ? 'x' : '-';
    out[3] = (perm & 0040)         ? 'r' : '-';
    out[4] = (perm & 0020)         ? 'w' : '-';
    out[5] = (perm & 0010)         ? 'x' : '-';
    out[6] = (perm & PERM_OTHER_R) ? 'r' : '-';
    out[7] = (perm & PERM_OTHER_W) ? 'w' : '-';
    out[8] = (perm & 0001)         ? 'x' : '-';
    out[9] = '\0';
}

/*
 * check_permission
 * Owner gets owner bits. Everyone else gets others bits.
 * Prints "Checking Permission..." and result — matches
 * the style shown in the assignment screenshot.
 */
int check_permission(const char *filename, int owner_bit, int other_bit) {
    if (!logged_in) { printf(RED "  Not logged in.\n" RESET); return 0; }
    char owner[MAX_NAME]; int perm = 0;
    if (!read_meta(filename, owner, &perm)) {
        printf(RED "  File '%s' not found or has no metadata.\n" RESET, filename);
        return 0;
    }
    printf("Checking Permission...\n");
    int allowed = (strcmp(owner, current_user.username) == 0)
                  ? (perm & owner_bit) : (perm & other_bit);
    if (allowed) { printf(GREEN "Permission Granted.\n\n" RESET); return 1; }
    else         { printf(RED   "Permission Denied.\n\n"  RESET); return 0; }
}

/* file path helper */
void file_path(const char *filename, char *out) {
    snprintf(out, 256, "%s/%s", FILES_DIR, filename);
}

/* ─────────────────────────────────────────────────
   XOR ENCRYPT / DECRYPT
   Same function encrypts and decrypts — XOR is
   symmetric: plaintext XOR key = ciphertext,
               ciphertext XOR key = plaintext.
───────────────────────────────────────────────── */
void xor_crypt(const char *in_path, const char *out_path) {
    FILE *in  = fopen(in_path,  "rb");
    FILE *out = fopen(out_path, "wb");
    if (!in || !out) {
        printf(RED "  Error: cannot open file for encryption.\n" RESET);
        if (in)  fclose(in);
        if (out) fclose(out);
        return;
    }
    int c;
    while ((c = fgetc(in)) != EOF) fputc(c ^ XOR_KEY, out);
    fclose(in); fclose(out);
}

/* ─────────────────────────────────────────────────
   AUTH SCREENS
───────────────────────────────────────────────── */
void screen_register(void) {
    header("REGISTER NEW USER");
    char uname[MAX_NAME], pw[MAX_PASS], confirm[MAX_PASS];

    input("Enter Username  : ", uname, sizeof(uname));
    input_password("Enter Password  : ", pw, sizeof(pw));
    input_password("Confirm Password: ", confirm, sizeof(confirm));

    if (strcmp(pw, confirm) != 0) {
        printf(RED "\n  Passwords do not match. Try again.\n" RESET);
        return;
    }

    User users[MAX_USERS]; int count = 0;
    load_users(users, &count);

    for (int i = 0; i < count; i++)
        if (strcmp(users[i].username, uname) == 0) {
            printf(RED "\n  Username '%s' already exists.\n" RESET, uname);
            return;
        }

    if (count >= MAX_USERS) {
        printf(RED "\n  User limit reached.\n" RESET); return;
    }

    strncpy(users[count].username, uname, MAX_NAME - 1);
    hash_password(pw, users[count].password_hash);
    users[count].is_owner = (count == 0) ? 1 : 0; /* first user = owner */
    count++;
    save_users(users, count);

    printf(GREEN "\n  Registration Successful! You can now log in.\n" RESET);
    audit("REGISTER", uname);
}

int screen_login(void) {
    header("LOGIN");
    char uname[MAX_NAME], pw[MAX_PASS];

    input("Enter Username: ", uname, sizeof(uname));
    input_password("Enter Password: ", pw, sizeof(pw));

    User users[MAX_USERS]; int count = 0;
    load_users(users, &count);
    char hashed[MAX_PASS];
    hash_password(pw, hashed);

    for (int i = 0; i < count; i++) {
        if (strcmp(users[i].username, uname) == 0 &&
            strcmp(users[i].password_hash, hashed) == 0) {
            current_user = users[i];
            logged_in = 1;
            printf(GREEN "\n  Login Successful. Welcome, %s!\n" RESET, uname);
            audit("LOGIN", NULL);
            return 1;
        }
    }
    printf(RED "\n  Login Failed: invalid username or password.\n" RESET);
    audit("LOGIN_FAIL", uname);
    return 0;
}

/* ─────────────────────────────────────────────────
   FILE OPERATION SCREENS
───────────────────────────────────────────────── */

/* CREATE */
void screen_create(void) {
    header("CREATE FILE");
    char fname[MAX_NAME], content[MAX_INPUT], perm_input[8];

    input("Enter Filename : ", fname, sizeof(fname));

    char fp[256]; file_path(fname, fp);
    if (access(fp, F_OK) == 0) {
        printf(RED "\n  A file with that name already exists.\n" RESET);
        return;
    }

    input("Enter Content  : ", content, sizeof(content));

    printf("\n");
    print_perm_guide();
    input("Enter Permissions [default 640]: ", perm_input, sizeof(perm_input));

    int perm = 0640; /* default — owner rw, group r, others none */
    if (strlen(perm_input) > 0)
        perm = (int)strtol(perm_input, NULL, 8);

    FILE *f = fopen(fp, "w");
    if (!f) { printf(RED "\n  Error: cannot create file.\n" RESET); return; }
    fprintf(f, "%s\n", content);
    fclose(f);

    write_meta(fname, current_user.username, perm);

    char pstr[10]; perm_to_str(perm, pstr);
    printf(GREEN "\n  File Created Successfully.\n" RESET);
    printf(DIM   "  Permissions : %s  (%o)\n" RESET, pstr, perm);
    printf(DIM   "  Owner       : %s\n" RESET, current_user.username);
    audit("CREATE", fname);
}

/* READ */
void screen_read(void) {
    header("READ FILE");
    char fname[MAX_NAME];
    input("Enter Filename: ", fname, sizeof(fname));
    printf("\n");

    if (!check_permission(fname, PERM_OWNER_R, PERM_OTHER_R)) {
        audit("READ_DENIED", fname); return;
    }

    char fp[256]; file_path(fname, fp);
    FILE *f = fopen(fp, "r");
    if (!f) { printf(RED "  File not found.\n" RESET); return; }

    printf(CYAN "── Contents of '%s' ──\n" RESET, fname);
    int c; while ((c = fgetc(f)) != EOF) putchar(c);
    printf(CYAN "────────────────────────\n" RESET);
    fclose(f);
    audit("READ", fname);
}

/* WRITE */
void screen_write(void) {
    header("WRITE TO FILE");
    char fname[MAX_NAME];
    input("Enter Filename: ", fname, sizeof(fname));
    printf("\n");

    if (!check_permission(fname, PERM_OWNER_W, PERM_OTHER_W)) {
        audit("WRITE_DENIED", fname); return;
    }

    char content[MAX_INPUT];
    input("Enter text: ", content, sizeof(content));

    char fp[256]; file_path(fname, fp);
    FILE *f = fopen(fp, "a");
    if (!f) { printf(RED "\n  Error opening file.\n" RESET); return; }
    fprintf(f, "%s\n", content);
    fclose(f);

    printf(GREEN "\n  Saved Successfully.\n" RESET);
    audit("WRITE", fname);
}

/* DELETE */
void screen_delete(void) {
    header("DELETE FILE");
    char fname[MAX_NAME];
    input("Enter Filename: ", fname, sizeof(fname));
    printf("\n");

    if (!check_permission(fname, PERM_OWNER_W, PERM_OTHER_W)) {
        audit("DELETE_DENIED", fname); return;
    }

    char confirm[4];
    input("Are you sure? (Y/N): ", confirm, sizeof(confirm));
    if (confirm[0] != 'Y' && confirm[0] != 'y') {
        printf(YELLOW "\n  Cancelled.\n" RESET); return;
    }

    char fp[256], mp[256];
    file_path(fname, fp); meta_path(fname, mp);
    remove(fp); remove(mp);

    printf(GREEN "\n  Deleted Successfully.\n" RESET);
    audit("DELETE", fname);
}

/* FILE PERMISSIONS — view and change */
void screen_permissions(void) {
    header("FILE PERMISSIONS");
    char fname[MAX_NAME];
    input("Enter Filename: ", fname, sizeof(fname));

    char owner[MAX_NAME]; int perm = 0;
    if (!read_meta(fname, owner, &perm)) {
        printf(RED "\n  File not found or has no metadata.\n" RESET); return;
    }

    char pstr[10]; perm_to_str(perm, pstr);
    printf("\n");
    printf("  File        : %s\n", fname);
    printf("  Owner       : %s\n", owner);
    printf("  Permissions : %s  (%o)\n\n", pstr, perm);

    /* only owner can change permissions */
    if (strcmp(owner, current_user.username) != 0) {
        printf(DIM "  Only the owner can change permissions.\n" RESET); return;
    }

    char choice[4];
    input("Change permissions? (Y/N): ", choice, sizeof(choice));
    if (choice[0] != 'Y' && choice[0] != 'y') return;

    printf("\n");
    print_perm_guide();

    char perm_input[8];
    input("New Permissions: ", perm_input, sizeof(perm_input));
    int new_perm = (int)strtol(perm_input, NULL, 8);
    write_meta(fname, owner, new_perm);

    perm_to_str(new_perm, pstr);
    printf(GREEN "\n  Permissions updated to %s (%o)\n" RESET, pstr, new_perm);
    audit("CHMOD", fname);
}

/* ENCRYPT */
void screen_encrypt(void) {
    header("ENCRYPT FILE");
    char fname[MAX_NAME];
    input("Enter Filename to encrypt: ", fname, sizeof(fname));
    printf("\n");

    if (!check_permission(fname, PERM_OWNER_R, PERM_OTHER_R)) {
        audit("ENCRYPT_DENIED", fname); return;
    }

    char fp[256], enc_name[256], enc_fp[256];
    file_path(fname, fp);
    snprintf(enc_name, sizeof(enc_name), "%s.enc", fname);
    file_path(enc_name, enc_fp);

    xor_crypt(fp, enc_fp);
    write_meta(enc_name, current_user.username, 0600);

    printf(GREEN "  Encrypted  : '%s'\n" RESET, fname);
    printf(GREEN "  Saved as   : '%s'\n" RESET, enc_name);
    printf(DIM   "  Permissions set to 600 (owner read/write only)\n" RESET);
    audit("ENCRYPT", fname);
}

/* DECRYPT */
void screen_decrypt(void) {
    header("DECRYPT FILE");
    char fname[MAX_NAME];
    input("Enter Filename to decrypt (.enc file): ", fname, sizeof(fname));
    printf("\n");

    if (!check_permission(fname, PERM_OWNER_R, PERM_OTHER_R)) {
        audit("DECRYPT_DENIED", fname); return;
    }

    char fp[256], dec_name[256], dec_fp[256];
    file_path(fname, fp);
    strncpy(dec_name, fname, sizeof(dec_name));
    char *dot = strstr(dec_name, ".enc");
    if (dot) *dot = '\0';
    strcat(dec_name, ".dec");
    file_path(dec_name, dec_fp);

    xor_crypt(fp, dec_fp);
    write_meta(dec_name, current_user.username, 0600);

    printf(GREEN "  Decrypted  : '%s'\n" RESET, fname);
    printf(GREEN "  Saved as   : '%s'\n" RESET, dec_name);
    audit("DECRYPT", fname);
}
