# Metadata Journaling for VSFS

## Overview

This project implements metadata journaling for a Very Simple File System (VSFS) to ensure crash consistency. The journal acts as a write-ahead log that records metadata changes before they are applied to the actual disk.

## Prerequisites

- GCC compiler
- Linux/Unix environment
- `vsfs.img` disk image (created by `mkfs`)

## Compilation

```bash
gcc -o journal journal.c -Wall
```

## Usage

### 1. Create a Fresh Disk Image

```bash
./mkfs
```

This creates `vsfs.img` with:
- 85 blocks (4KB each)
- 64 inodes
- 16-block journal
- Root directory

### 2. Create Files (Log to Journal)

```bash
./journal create <filename>
```

**Example:**
```bash
./journal create myfile.txt
./journal create test.c
```

This logs the file creation to the journal WITHOUT modifying the actual disk.

### 3. Apply Changes (Install from Journal)

```bash
./journal install
```

This reads the journal and applies all committed transactions to the disk, then clears the journal.

### 4. Verify Filesystem (Optional)

```bash
./validator
```

Checks if the filesystem is consistent (all bitmaps, inodes, and directories match).

## Complete Workflow Example

```bash
# Step 1: Create fresh filesystem
./mkfs

# Step 2: Create multiple files
./journal create file1.txt
./journal create file2.txt
./journal create file3.txt

# Step 3: Apply all changes
./journal install

# Step 4: Verify consistency
./validator
```

## Commands

| Command | Description |
|---------|-------------|
| `./journal create <filename>` | Log file creation to journal |
| `./journal install` | Apply journal changes to disk |
| `./validator` | Verify filesystem consistency |
| `./mkfs` | Create new filesystem image |

## How It Works

### Create Command

1. Reads current metadata (bitmaps, inode table, directory)
2. Finds free inode and directory slot
3. Prepares updated versions in memory
4. Writes **DATA records** to journal:
   - Inode bitmap
   - Inode table block(s)
   - Root directory
5. Writes **COMMIT record** to finalize transaction
6. Does NOT modify actual disk yet

### Install Command

1. Reads journal header
2. Scans through all records
3. For each transaction (DATA records + COMMIT):
   - Applies updates to actual disk blocks
4. Discards incomplete transactions (no COMMIT)
5. Clears journal

## Limitations

- Maximum 63 files (64 inodes - 1 for root)
- Filename max length: 27 characters
- Root directory only (no subdirectories)
- Journal holds ~5 transactions before needing install

## Error Messages

| Error | Cause | Solution |
|-------|-------|----------|
| `Journal is full` | Too many uncommitted transactions | Run `./journal install` |
| `No free inodes available` | All 63 file slots used | Cannot create more files |
| `File already exists` | Duplicate filename | Choose different name |
| `Filename too long` | Name exceeds 27 characters | Shorten filename |
| `Cannot open vsfs.img` | Missing disk image | Run `./mkfs` first |

## Testing Scenarios

### Test 1: Basic Operation
```bash
./mkfs
./journal create test.txt
./journal install
./validator
```

### Test 2: Multiple Files
```bash
./mkfs
./journal create file1.txt
./journal create file2.txt
./journal create file3.txt
./journal install
./validator
```

### Test 3: Journal Full
```bash
./mkfs
./journal create f1.txt
./journal create f2.txt
./journal create f3.txt
./journal create f4.txt
./journal create f5.txt
./journal create f6.txt  # Should fail with "Journal is full"
./journal install         # Clear journal
./journal create f6.txt   # Now succeeds
```

### Test 4: Crash Simulation
```bash
./mkfs
./journal create file1.txt
# Simulate crash - don't run install
# Journal has uncommitted data
./journal install  # Recovers by discarding incomplete transaction
./validator        # Should still be consistent
```

## Project Structure

```
journal.c       - Main journaling implementation
mkfs.c          - Filesystem creator
validator.c     - Consistency checker
vsfs.img        - Disk image (created by mkfs)
```

## Notes

- Always run `./journal install` to apply changes
- Journal is append-only until install
- Validator checks for consistency after operations
- Use `./mkfs` to reset to clean state
