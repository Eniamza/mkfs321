/*
 * journal.c - Metadata Journaling for VSFS
 * CSE321 Term Project
 * 
 * Commands:
 *   ./journal create <filename>  - Log file creation to journal
 *   ./journal install            - Apply journal to disk
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* ==================== CONSTANTS ==================== */

#define BLOCK_SIZE      4096
#define TOTAL_BLOCKS    85
#define JOURNAL_BLOCKS  16
#define MAX_INODES      64
#define MAX_DIRENTS     (BLOCK_SIZE / 32)  /* 128 entries per block */

/* Magic Numbers */
#define SUPERBLOCK_MAGIC  0x56534653  /* "VSFS" */
#define JOURNAL_MAGIC     0x4A524E4C  /* "JRNL" */

/* Record Types */
#define REC_DATA    1
#define REC_COMMIT  2

/* Inode Types */
#define INODE_FREE  0
#define INODE_FILE  1
#define INODE_DIR   2

/* Disk Image File */
#define DISK_IMAGE  "vsfs.img"

/* ==================== DATA STRUCTURES ==================== */

/* Superblock - Block 0 */
struct superblock {
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t journal_block;   /* Start of journal */
    uint32_t inode_bitmap;    /* Inode bitmap block */
    uint32_t data_bitmap;     /* Data bitmap block */
    uint32_t inode_start;     /* First inode table block */
    uint32_t data_start;      /* First data block */
    uint8_t _pad[BLOCK_SIZE - 9 * 4];
};

/* Journal Header - At start of journal */
struct journal_header {
    uint32_t magic;
    uint32_t nbytes_used;     /* How many bytes used in journal */
};

/* Record Header - Starts every record */
struct rec_header {
    uint16_t type;
    uint16_t size;            /* Total size of record */
};

/* Data Record - Logs a block update */
struct data_record {
    struct rec_header hdr;
    uint32_t block_no;
    uint8_t data[BLOCK_SIZE];
};

/* Commit Record - Ends a transaction */
struct commit_record {
    struct rec_header hdr;
};

/* Directory Entry */
struct dirent {
    uint32_t inode;
    char name[28];
};

/* Inode - must be exactly 128 bytes */
struct inode {
    uint16_t type;      /* 2 bytes */
    uint16_t links;     /* 2 bytes */
    uint32_t size;      /* 4 bytes */
    uint32_t direct[8]; /* 32 bytes */
    uint32_t ctime;     /* 4 bytes */
    uint32_t mtime;     /* 4 bytes */
    uint8_t _pad[80];   /* 80 bytes = 128 - 48 total */
};

/* ==================== GLOBAL VARIABLES ==================== */

FILE *disk = NULL;
struct superblock sb;

/* ==================== HELPER FUNCTIONS ==================== */

/* Read a block from disk */
int read_block(uint32_t block_no, void *buffer) {
    if (fseek(disk, block_no * BLOCK_SIZE, SEEK_SET) != 0) {
        printf("Error: Cannot seek to block %u\n", block_no);
        return -1;
    }
    if (fread(buffer, BLOCK_SIZE, 1, disk) != 1) {
        printf("Error: Cannot read block %u\n", block_no);
        return -1;
    }
    return 0;
}

/* Write a block to disk */
int write_block(uint32_t block_no, void *buffer) {
    if (fseek(disk, block_no * BLOCK_SIZE, SEEK_SET) != 0) {
        printf("Error: Cannot seek to block %u\n", block_no);
        return -1;
    }
    if (fwrite(buffer, BLOCK_SIZE, 1, disk) != 1) {
        printf("Error: Cannot write block %u\n", block_no);
        return -1;
    }
    fflush(disk);
    return 0;
}

/* Read bytes from journal at given offset */
int read_journal_bytes(uint32_t offset, void *buffer, uint32_t size) {
    uint32_t journal_start = sb.journal_block * BLOCK_SIZE;
    if (fseek(disk, journal_start + offset, SEEK_SET) != 0) {
        return -1;
    }
    if (fread(buffer, size, 1, disk) != 1) {
        return -1;
    }
    return 0;
}

/* Write bytes to journal at given offset */
int write_journal_bytes(uint32_t offset, void *buffer, uint32_t size) {
    uint32_t journal_start = sb.journal_block * BLOCK_SIZE;
    if (fseek(disk, journal_start + offset, SEEK_SET) != 0) {
        return -1;
    }
    if (fwrite(buffer, size, 1, disk) != 1) {
        return -1;
    }
    fflush(disk);
    return 0;
}

/* Open disk and read superblock */
int open_disk(void) {
    disk = fopen(DISK_IMAGE, "r+b");
    if (disk == NULL) {
        printf("Error: Cannot open %s\n", DISK_IMAGE);
        return -1;
    }
    
    /* Read superblock */
    if (read_block(0, &sb) != 0) {
        printf("Error: Cannot read superblock\n");
        fclose(disk);
        return -1;
    }
    
    /* Verify magic number */
    if (sb.magic != SUPERBLOCK_MAGIC) {
        printf("Error: Invalid filesystem (bad magic number)\n");
        fclose(disk);
        return -1;
    }
    
    return 0;
}

/* Close disk */
void close_disk(void) {
    if (disk != NULL) {
        fclose(disk);
        disk = NULL;
    }
}

/* Find first free bit in bitmap, return index or -1 */
int find_free_bit(uint8_t *bitmap, int max_bits) {
    int i, j;
    for (i = 0; i < max_bits / 8; i++) {
        if (bitmap[i] != 0xFF) {
            /* Found a byte with a free bit */
            for (j = 0; j < 8; j++) {
                if ((bitmap[i] & (1 << j)) == 0) {
                    return i * 8 + j;
                }
            }
        }
    }
    return -1;
}

/* Set a bit in bitmap */
void set_bit(uint8_t *bitmap, int index) {
    bitmap[index / 8] |= (1 << (index % 8));
}

/* ==================== CREATE COMMAND ==================== */

#define INODES_PER_BLOCK  (BLOCK_SIZE / 128)  /* 32 inodes per block */

int journal_create(const char *filename) {
    struct journal_header jh;
    uint8_t inode_bitmap[BLOCK_SIZE];
    uint8_t inode_block0[BLOCK_SIZE];  /* First inode block (inodes 0-31) */
    uint8_t inode_block1[BLOCK_SIZE];  /* Second inode block (inodes 32-63) */
    uint8_t dir_block[BLOCK_SIZE];
    struct inode *inodes0;
    struct inode *inodes1;
    struct dirent *dirents;
    int free_inode, free_dirent;
    int i;
    uint32_t current_time;
    uint32_t journal_space;
    uint32_t needed_space;
    uint32_t offset;
    struct data_record data_rec;
    struct commit_record commit_rec;
    int inode_in_block1;  /* Is the new inode in the second block? */
    uint32_t root_dir_block;
    
    /* Check filename length */
    if (strlen(filename) > 27) {
        printf("Error: Filename too long (max 27 characters)\n");
        return -1;
    }
    
    /* Read journal header */
    if (read_journal_bytes(0, &jh, sizeof(jh)) != 0) {
        printf("Error: Cannot read journal header\n");
        return -1;
    }
    
    /* Initialize journal if magic is wrong */
    if (jh.magic != JOURNAL_MAGIC) {
        jh.magic = JOURNAL_MAGIC;
        jh.nbytes_used = sizeof(struct journal_header);
    }
    
    /* Check if we have enough journal space */
    /* Need: up to 4 data records (inode bitmap, 2 inode table blocks, directory) + 1 commit */
    journal_space = JOURNAL_BLOCKS * BLOCK_SIZE;
    needed_space = 4 * sizeof(struct data_record) + sizeof(struct commit_record);
    
    if (jh.nbytes_used + needed_space > journal_space) {
        printf("Error: Journal is full. Please run './journal install' first.\n");
        return -1;
    }
    
    /* Read inode bitmap */
    if (read_block(sb.inode_bitmap, inode_bitmap) != 0) {
        printf("Error: Cannot read inode bitmap\n");
        return -1;
    }
    
    /* Find free inode (skip inode 0 which is root) */
    free_inode = -1;
    for (i = 1; i < MAX_INODES; i++) {
        if ((inode_bitmap[i / 8] & (1 << (i % 8))) == 0) {
            free_inode = i;
            break;
        }
    }
    
    if (free_inode == -1) {
        printf("Error: No free inodes available\n");
        return -1;
    }
    
    /* Determine which inode block the new inode is in */
    inode_in_block1 = (free_inode >= INODES_PER_BLOCK);
    
    /* Read BOTH inode table blocks */
    if (read_block(sb.inode_start, inode_block0) != 0) {
        printf("Error: Cannot read inode table block 0\n");
        return -1;
    }
    if (read_block(sb.inode_start + 1, inode_block1) != 0) {
        printf("Error: Cannot read inode table block 1\n");
        return -1;
    }
    inodes0 = (struct inode *)inode_block0;
    inodes1 = (struct inode *)inode_block1;
    
    /* Read root directory (inode 0, first direct block) */
    root_dir_block = inodes0[0].direct[0];
    if (read_block(root_dir_block, dir_block) != 0) {
        printf("Error: Cannot read root directory\n");
        return -1;
    }
    dirents = (struct dirent *)dir_block;
    
    /* Check if filename already exists and find free slot */
    /* Note: A slot is "used" if it has a non-empty name */
    /* Slots 0 and 1 are "." and ".." - skip them */
    free_dirent = -1;
    for (i = 0; i < MAX_DIRENTS; i++) {
        /* Check if this slot has a valid entry (non-empty name) */
        if (dirents[i].name[0] != '\0') {
            /* Check for duplicate filename */
            if (strcmp(dirents[i].name, filename) == 0) {
                printf("Error: File '%s' already exists\n", filename);
                return -1;
            }
        } else if (free_dirent == -1) {
            /* Found an empty slot (name is empty) */
            free_dirent = i;
        }
    }
    
    if (free_dirent == -1) {
        printf("Error: Root directory is full\n");
        return -1;
    }
    
    /* Now prepare the updated blocks in memory */
    
    /* Update inode bitmap */
    set_bit(inode_bitmap, free_inode);
    
    /* Update inode table - create new file inode */
    current_time = (uint32_t)time(NULL);
    
    if (inode_in_block1) {
        /* Inode is in second block (inodes 32-63) */
        int idx = free_inode - INODES_PER_BLOCK;
        inodes1[idx].type = INODE_FILE;
        inodes1[idx].links = 1;
        inodes1[idx].size = 0;
        for (i = 0; i < 8; i++) {
            inodes1[idx].direct[i] = 0;
        }
        inodes1[idx].ctime = current_time;
        inodes1[idx].mtime = current_time;
    } else {
        /* Inode is in first block (inodes 0-31) */
        inodes0[free_inode].type = INODE_FILE;
        inodes0[free_inode].links = 1;
        inodes0[free_inode].size = 0;
        for (i = 0; i < 8; i++) {
            inodes0[free_inode].direct[i] = 0;
        }
        inodes0[free_inode].ctime = current_time;
        inodes0[free_inode].mtime = current_time;
    }
    
    /* Update directory entry */
    dirents[free_dirent].inode = free_inode;
    memset(dirents[free_dirent].name, 0, 28);
    strncpy(dirents[free_dirent].name, filename, 27);
    
    /* Update root inode size to include new entry */
    /* Size = (highest used entry index + 1) * sizeof(dirent) */
    /* Find the highest used entry */
    {
        int highest_used = -1;
        for (i = 0; i < MAX_DIRENTS; i++) {
            if (dirents[i].name[0] != '\0') {
                highest_used = i;
            }
        }
        if (highest_used >= 0) {
            inodes0[0].size = (highest_used + 1) * sizeof(struct dirent);
        }
    }
    
    /* Now write to journal */
    offset = jh.nbytes_used;
    
    /* DATA record 1: Inode Bitmap */
    data_rec.hdr.type = REC_DATA;
    data_rec.hdr.size = sizeof(struct data_record);
    data_rec.block_no = sb.inode_bitmap;
    memcpy(data_rec.data, inode_bitmap, BLOCK_SIZE);
    
    if (write_journal_bytes(offset, &data_rec, sizeof(data_rec)) != 0) {
        printf("Error: Cannot write to journal\n");
        return -1;
    }
    offset += sizeof(struct data_record);
    
    /* DATA record 2: Inode Table Block 0 (always written - contains root inode) */
    data_rec.hdr.type = REC_DATA;
    data_rec.hdr.size = sizeof(struct data_record);
    data_rec.block_no = sb.inode_start;
    memcpy(data_rec.data, inode_block0, BLOCK_SIZE);
    
    if (write_journal_bytes(offset, &data_rec, sizeof(data_rec)) != 0) {
        printf("Error: Cannot write to journal\n");
        return -1;
    }
    offset += sizeof(struct data_record);
    
    /* DATA record 3: Inode Table Block 1 (if inode is in second block) */
    if (inode_in_block1) {
        data_rec.hdr.type = REC_DATA;
        data_rec.hdr.size = sizeof(struct data_record);
        data_rec.block_no = sb.inode_start + 1;
        memcpy(data_rec.data, inode_block1, BLOCK_SIZE);
        
        if (write_journal_bytes(offset, &data_rec, sizeof(data_rec)) != 0) {
            printf("Error: Cannot write to journal\n");
            return -1;
        }
        offset += sizeof(struct data_record);
    }
    
    /* DATA record: Root Directory Block */
    data_rec.hdr.type = REC_DATA;
    data_rec.hdr.size = sizeof(struct data_record);
    data_rec.block_no = root_dir_block;
    memcpy(data_rec.data, dir_block, BLOCK_SIZE);
    
    if (write_journal_bytes(offset, &data_rec, sizeof(data_rec)) != 0) {
        printf("Error: Cannot write to journal\n");
        return -1;
    }
    offset += sizeof(struct data_record);
    
    /* COMMIT record */
    commit_rec.hdr.type = REC_COMMIT;
    commit_rec.hdr.size = sizeof(struct commit_record);
    
    if (write_journal_bytes(offset, &commit_rec, sizeof(commit_rec)) != 0) {
        printf("Error: Cannot write commit record\n");
        return -1;
    }
    offset += sizeof(struct commit_record);
    
    /* Update journal header */
    jh.nbytes_used = offset;
    if (write_journal_bytes(0, &jh, sizeof(jh)) != 0) {
        printf("Error: Cannot update journal header\n");
        return -1;
    }
    
    printf("Success: File '%s' logged to journal (inode %d)\n", filename, free_inode);
    printf("Run './journal install' to apply changes to disk.\n");
    
    return 0;
}

/* ==================== INSTALL COMMAND ==================== */

int journal_install(void) {
    struct journal_header jh;
    uint8_t *journal_data;
    uint32_t offset;
    struct rec_header *rec;
    struct data_record *data_rec;
    int transactions = 0;
    int pending_writes = 0;
    uint32_t write_blocks[16];
    uint8_t *write_data[16];
    int i;
    
    /* Read journal header */
    if (read_journal_bytes(0, &jh, sizeof(jh)) != 0) {
        printf("Error: Cannot read journal header\n");
        return -1;
    }
    
    /* Check if journal is valid */
    if (jh.magic != JOURNAL_MAGIC) {
        printf("Journal is empty or uninitialized.\n");
        return 0;
    }
    
    /* Check if there's anything to install */
    if (jh.nbytes_used <= sizeof(struct journal_header)) {
        printf("Journal is empty. Nothing to install.\n");
        return 0;
    }
    
    /* Allocate buffer for journal contents */
    journal_data = malloc(jh.nbytes_used);
    if (journal_data == NULL) {
        printf("Error: Cannot allocate memory\n");
        return -1;
    }
    
    /* Read entire used journal */
    if (read_journal_bytes(0, journal_data, jh.nbytes_used) != 0) {
        printf("Error: Cannot read journal data\n");
        free(journal_data);
        return -1;
    }
    
    /* Process journal records */
    offset = sizeof(struct journal_header);
    pending_writes = 0;
    
    while (offset < jh.nbytes_used) {
        rec = (struct rec_header *)(journal_data + offset);
        
        /* Safety check */
        if (rec->size == 0 || offset + rec->size > jh.nbytes_used) {
            printf("Warning: Corrupted record at offset %u, stopping.\n", offset);
            break;
        }
        
        if (rec->type == REC_DATA) {
            /* Store data record for later writing */
            data_rec = (struct data_record *)(journal_data + offset);
            
            if (pending_writes < 16) {
                write_blocks[pending_writes] = data_rec->block_no;
                write_data[pending_writes] = malloc(BLOCK_SIZE);
                if (write_data[pending_writes] != NULL) {
                    memcpy(write_data[pending_writes], data_rec->data, BLOCK_SIZE);
                    pending_writes++;
                }
            }
            
            offset += sizeof(struct data_record);
        }
        else if (rec->type == REC_COMMIT) {
            /* Found commit - apply all pending writes */
            for (i = 0; i < pending_writes; i++) {
                if (write_block(write_blocks[i], write_data[i]) != 0) {
                    printf("Error: Failed to write block %u\n", write_blocks[i]);
                }
                free(write_data[i]);
            }
            
            transactions++;
            pending_writes = 0;
            offset += sizeof(struct commit_record);
        }
        else {
            /* Unknown record type - skip */
            printf("Warning: Unknown record type %u at offset %u\n", rec->type, offset);
            break;
        }
    }
    
    /* Discard any uncommitted data */
    if (pending_writes > 0) {
        printf("Warning: Discarding %d uncommitted writes (no COMMIT found)\n", pending_writes);
        for (i = 0; i < pending_writes; i++) {
            free(write_data[i]);
        }
    }
    
    /* Clear journal */
    jh.nbytes_used = sizeof(struct journal_header);
    if (write_journal_bytes(0, &jh, sizeof(jh)) != 0) {
        printf("Error: Cannot reset journal header\n");
        free(journal_data);
        return -1;
    }
    
    free(journal_data);
    
    printf("Success: Installed %d transaction(s) from journal.\n", transactions);
    printf("Journal has been cleared.\n");
    
    return 0;
}

/* ==================== MAIN FUNCTION ==================== */

int main(int argc, char *argv[]) {
    int result = 0;
    
    /* Check arguments */
    if (argc < 2) {
        printf("Usage:\n");
        printf("  %s create <filename>  - Create a new file (logged to journal)\n", argv[0]);
        printf("  %s install            - Apply journal to disk\n", argv[0]);
        return 1;
    }
    
    /* Open disk image */
    if (open_disk() != 0) {
        return 1;
    }
    
    /* Process command */
    if (strcmp(argv[1], "create") == 0) {
        if (argc < 3) {
            printf("Error: Missing filename\n");
            printf("Usage: %s create <filename>\n", argv[0]);
            result = 1;
        } else {
            result = journal_create(argv[2]);
        }
    }
    else if (strcmp(argv[1], "install") == 0) {
        result = journal_install();
    }
    else {
        printf("Error: Unknown command '%s'\n", argv[1]);
        printf("Use 'create' or 'install'\n");
        result = 1;
    }
    
    /* Cleanup */
    close_disk();
    
    return (result == 0) ? 0 : 1;
}
