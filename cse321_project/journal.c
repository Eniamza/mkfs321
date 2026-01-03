#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define BLOCK_SIZE      4096
#define JOURNAL_BLOCKS  16
#define MAX_INODES      64
#define INODES_PER_BLOCK 32
#define MAX_DIRENTS     128

#define SUPERBLOCK_MAGIC  0x56534653
#define JOURNAL_MAGIC     0x4A524E4C

#define REC_DATA    1
#define REC_COMMIT  2

#define INODE_FILE  1

#define TRUE  1
#define FALSE 0

#define DISK_IMAGE  "vsfs.img"

struct superblock {
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t journal_block;
    uint32_t inode_bitmap;
    uint32_t data_bitmap;
    uint32_t inode_start;
    uint32_t data_start;
    uint8_t _pad[BLOCK_SIZE - 9 * 4];
};

struct journal_header {
    uint32_t magic;
    uint32_t nbytes_used;
};

struct rec_header {
    uint16_t type;
    uint16_t size;
};

struct data_record {
    struct rec_header hdr;
    uint32_t block_no;
    uint8_t data[BLOCK_SIZE];
};

struct commit_record {
    struct rec_header hdr;
};

struct dirent {
    uint32_t inode;
    char name[28];
};

struct inode {
    uint16_t type;
    uint16_t links;
    uint32_t size;
    uint32_t direct[8];
    uint32_t ctime;
    uint32_t mtime;
    uint8_t _pad[80];
};

FILE *disk = NULL;
struct superblock sb;

int read_block(uint32_t block_no, void *buffer) {
    fseek(disk, block_no * BLOCK_SIZE, SEEK_SET);
    if (fread(buffer, BLOCK_SIZE, 1, disk) != 1) {
        return FALSE;
    }
    return TRUE;
}

int write_block(uint32_t block_no, void *buffer) {
    fseek(disk, block_no * BLOCK_SIZE, SEEK_SET);
    if (fwrite(buffer, BLOCK_SIZE, 1, disk) != 1) {
        return FALSE;
    }
    fflush(disk);
    return TRUE;
}

int read_journal(uint32_t offset, void *buffer, uint32_t size) {
    uint32_t pos = sb.journal_block * BLOCK_SIZE + offset;
    fseek(disk, pos, SEEK_SET);
    if (fread(buffer, size, 1, disk) != 1) {
        return FALSE;
    }
    return TRUE;
}

int write_journal(uint32_t offset, void *buffer, uint32_t size) {
    uint32_t pos = sb.journal_block * BLOCK_SIZE + offset;
    fseek(disk, pos, SEEK_SET);
    if (fwrite(buffer, size, 1, disk) != 1) {
        return FALSE;
    }
    fflush(disk);
    return TRUE;
}

int open_disk(void) {
    disk = fopen(DISK_IMAGE, "r+b");
    if (disk == NULL) {
        printf("Error: Cannot open %s\n", DISK_IMAGE);
        return FALSE;
    }
    
    read_block(0, &sb);
    
    if (sb.magic != SUPERBLOCK_MAGIC) {
        printf("Error: Invalid filesystem\n");
        fclose(disk);
        return FALSE;
    }
    return TRUE;
}

void close_disk(void) {
    if (disk != NULL) {
        fclose(disk);
        disk = NULL;
    }
}

int find_free_inode(uint8_t *bitmap) {
    int i, j;
    for (i = 0; i < MAX_INODES / 8; i++) {
        if (bitmap[i] == 0xFF) continue;
        for (j = 0; j < 8; j++) {
            int idx = i * 8 + j;
            if (idx == 0) continue;
            if ((bitmap[i] & (1 << j)) == 0) {
                return idx;
            }
        }
    }
    return -1;
}

void set_bit(uint8_t *bitmap, int idx) {
    bitmap[idx / 8] |= (1 << (idx % 8));
}

int find_free_dirent(struct dirent *dirents, const char *filename, int *slot) {
    int i;
    int found_slot = FALSE;
    *slot = -1;
    
    for (i = 0; i < MAX_DIRENTS; i++) {
        if (dirents[i].name[0] != '\0') {
            if (strcmp(dirents[i].name, filename) == 0) {
                return -1;
            }
        } else {
            if (found_slot == FALSE) {
                *slot = i;
                found_slot = TRUE;
            }
        }
    }
    return found_slot ? 0 : -2;
}

int get_highest_dirent(struct dirent *dirents) {
    int i;
    int highest = -1;
    for (i = 0; i < MAX_DIRENTS; i++) {
        if (dirents[i].name[0] != '\0') {
            highest = i;
        }
    }
    return highest;
}

int journal_create(const char *filename) {
    struct journal_header jh;
    uint8_t inode_bitmap[BLOCK_SIZE];
    uint8_t inode_block0[BLOCK_SIZE];
    uint8_t inode_block1[BLOCK_SIZE];
    uint8_t dir_block[BLOCK_SIZE];
    struct inode *inodes0;
    struct inode *inodes1;
    struct dirent *dirents;
    struct data_record data_rec;
    struct commit_record commit_rec;
    
    int free_inode;
    int free_slot;
    int in_block1;
    int result;
    uint32_t offset;
    uint32_t root_dir_block;
    uint32_t current_time;
    uint32_t space_needed;
    uint32_t space_available;
    int i;
    int highest;
    
    if (strlen(filename) > 27) {
        printf("Error: Filename too long (max 27 characters)\n");
        return FALSE;
    }
    
    read_journal(0, &jh, sizeof(jh));
    
    if (jh.magic != JOURNAL_MAGIC) {
        jh.magic = JOURNAL_MAGIC;
        jh.nbytes_used = sizeof(struct journal_header);
    }
    
    space_available = JOURNAL_BLOCKS * BLOCK_SIZE;
    space_needed = 4 * sizeof(struct data_record) + sizeof(struct commit_record);
    
    if (jh.nbytes_used + space_needed > space_available) {
        printf("Error: Journal is full. Please run './journal install' first.\n");
        return FALSE;
    }
    
    read_block(sb.inode_bitmap, inode_bitmap);
    
    free_inode = find_free_inode(inode_bitmap);
    if (free_inode == -1) {
        printf("Error: No free inodes available\n");
        return FALSE;
    }
    
    in_block1 = (free_inode >= INODES_PER_BLOCK) ? TRUE : FALSE;
    
    read_block(sb.inode_start, inode_block0);
    read_block(sb.inode_start + 1, inode_block1);
    
    inodes0 = (struct inode *)inode_block0;
    inodes1 = (struct inode *)inode_block1;
    
    root_dir_block = inodes0[0].direct[0];
    read_block(root_dir_block, dir_block);
    dirents = (struct dirent *)dir_block;
    
    result = find_free_dirent(dirents, filename, &free_slot);
    
    if (result == -1) {
        printf("Error: File '%s' already exists\n", filename);
        return FALSE;
    }
    if (result == -2 || free_slot == -1) {
        printf("Error: Root directory is full\n");
        return FALSE;
    }
    
    set_bit(inode_bitmap, free_inode);
    
    current_time = (uint32_t)time(NULL);
    
    if (in_block1 == TRUE) {
        int idx = free_inode - INODES_PER_BLOCK;
        inodes1[idx].type = INODE_FILE;
        inodes1[idx].links = 1;
        inodes1[idx].size = 0;
        for (i = 0; i < 8; i++) inodes1[idx].direct[i] = 0;
        inodes1[idx].ctime = current_time;
        inodes1[idx].mtime = current_time;
    } else {
        inodes0[free_inode].type = INODE_FILE;
        inodes0[free_inode].links = 1;
        inodes0[free_inode].size = 0;
        for (i = 0; i < 8; i++) inodes0[free_inode].direct[i] = 0;
        inodes0[free_inode].ctime = current_time;
        inodes0[free_inode].mtime = current_time;
    }
    
    dirents[free_slot].inode = free_inode;
    memset(dirents[free_slot].name, 0, 28);
    strncpy(dirents[free_slot].name, filename, 27);
    
    highest = get_highest_dirent(dirents);
    if (highest >= 0) {
        inodes0[0].size = (highest + 1) * sizeof(struct dirent);
    }
    
    offset = jh.nbytes_used;
    
    data_rec.hdr.type = REC_DATA;
    data_rec.hdr.size = sizeof(struct data_record);
    data_rec.block_no = sb.inode_bitmap;
    memcpy(data_rec.data, inode_bitmap, BLOCK_SIZE);
    write_journal(offset, &data_rec, sizeof(data_rec));
    offset += sizeof(struct data_record);
    
    data_rec.block_no = sb.inode_start;
    memcpy(data_rec.data, inode_block0, BLOCK_SIZE);
    write_journal(offset, &data_rec, sizeof(data_rec));
    offset += sizeof(struct data_record);
    
    if (in_block1 == TRUE) {
        data_rec.block_no = sb.inode_start + 1;
        memcpy(data_rec.data, inode_block1, BLOCK_SIZE);
        write_journal(offset, &data_rec, sizeof(data_rec));
        offset += sizeof(struct data_record);
    }
    
    data_rec.block_no = root_dir_block;
    memcpy(data_rec.data, dir_block, BLOCK_SIZE);
    write_journal(offset, &data_rec, sizeof(data_rec));
    offset += sizeof(struct data_record);
    
    commit_rec.hdr.type = REC_COMMIT;
    commit_rec.hdr.size = sizeof(struct commit_record);
    write_journal(offset, &commit_rec, sizeof(commit_rec));
    offset += sizeof(struct commit_record);
    
    jh.nbytes_used = offset;
    write_journal(0, &jh, sizeof(jh));
    
    printf("Success: File '%s' logged to journal (inode %d)\n", filename, free_inode);
    printf("Run './journal install' to apply changes to disk.\n");
    
    return TRUE;
}

int journal_install(void) {
    struct journal_header jh;
    uint8_t *journal_data;
    struct rec_header *rec;
    struct data_record *data_rec;
    
    uint32_t offset;
    int transactions = 0;
    int pending = 0;
    int i;
    
    uint32_t write_blocks[16];
    uint8_t *write_data[16];
    
    read_journal(0, &jh, sizeof(jh));
    
    if (jh.magic != JOURNAL_MAGIC) {
        printf("Journal is empty or uninitialized.\n");
        return TRUE;
    }
    
    if (jh.nbytes_used <= sizeof(struct journal_header)) {
        printf("Journal is empty. Nothing to install.\n");
        return TRUE;
    }
    
    journal_data = malloc(jh.nbytes_used);
    if (journal_data == NULL) {
        printf("Error: Cannot allocate memory\n");
        return FALSE;
    }
    
    read_journal(0, journal_data, jh.nbytes_used);
    
    offset = sizeof(struct journal_header);
    pending = 0;
    
    while (offset < jh.nbytes_used) {
        rec = (struct rec_header *)(journal_data + offset);
        
        if (rec->size == 0) break;
        if (offset + rec->size > jh.nbytes_used) break;
        
        if (rec->type == REC_DATA) {
            data_rec = (struct data_record *)(journal_data + offset);
            
            if (pending < 16) {
                write_blocks[pending] = data_rec->block_no;
                write_data[pending] = malloc(BLOCK_SIZE);
                memcpy(write_data[pending], data_rec->data, BLOCK_SIZE);
                pending++;
            }
            offset += sizeof(struct data_record);
        }
        else if (rec->type == REC_COMMIT) {
            for (i = 0; i < pending; i++) {
                write_block(write_blocks[i], write_data[i]);
                free(write_data[i]);
            }
            transactions++;
            pending = 0;
            offset += sizeof(struct commit_record);
        }
        else {
            break;
        }
    }
    
    if (pending > 0) {
        printf("Warning: Discarding %d uncommitted writes\n", pending);
        for (i = 0; i < pending; i++) {
            free(write_data[i]);
        }
    }
    
    jh.nbytes_used = sizeof(struct journal_header);
    write_journal(0, &jh, sizeof(jh));
    
    free(journal_data);
    
    printf("Success: Installed %d transaction(s) from journal.\n", transactions);
    printf("Journal has been cleared.\n");
    
    return TRUE;
}

int main(int argc, char *argv[]) {
    int result;
    
    if (argc < 2) {
        printf("Usage:\n");
        printf("  %s create <filename>\n", argv[0]);
        printf("  %s install\n", argv[0]);
        return 1;
    }
    
    if (open_disk() == FALSE) {
        return 1;
    }
    
    if (strcmp(argv[1], "create") == 0) {
        if (argc < 3) {
            printf("Error: Missing filename\n");
            result = FALSE;
        } else {
            result = journal_create(argv[2]);
        }
    }
    else if (strcmp(argv[1], "install") == 0) {
        result = journal_install();
    }
    else {
        printf("Error: Unknown command '%s'\n", argv[1]);
        result = FALSE;
    }
    
    close_disk();
    
    return (result == TRUE) ? 0 : 1;
}
