#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/buffer_head.h> // For sb_bread and buffer_head

/* Use your v6 magic number: 'VLX6' */
#define VELOXFS_MAGIC 0x564C5836

static struct tree_descr velox_files[] = { { \0 } };

/* This lives in RAM while the file is open */
struct veloxfs_inode_info {
    struct veloxfs_inode v_inode; // Your 144-byte disk structure
    struct inode vfs_inode;       // The standard Linux inode
};

/* Helper to get our private data from a generic VFS inode */
static inline struct veloxfs_inode_info *VELOXFS_I(struct inode *inode) {
    return container_of(inode, struct veloxfs_inode_info, vfs_inode);
}

static const struct super_operations veloxfs_sops = {
    .alloc_inode   = veloxfs_alloc_inode,   /* Custom slab allocator for vi_info */
    .destroy_inode = veloxfs_destroy_inode, /* Free the slab entry */
    .write_inode   = veloxfs_write_inode,   /* Commit 144-byte struct to disk */
    .evict_inode   = veloxfs_evict_inode,   /* Final cleanup / block reclamation */
    .put_super     = veloxfs_put_super,     /* Unmount cleanup */
    .statfs        = veloxfs_statfs,        /* Reports 'df' usage stats */
    /* .drop_inode can be added if you want custom caching logic */
};

static const struct inode_operations veloxfs_file_inode_operations = {
    .setattr    = veloxfs_setattr,    /* Truncate and permission changes */
    .getattr    = veloxfs_getattr,    /* Standard stat info */
};

static const struct file_operations veloxfs_file_operations = {
    .read_iter  = generic_file_read_iter,  /* Handled by page cache/aops */
    .write_iter = generic_file_write_iter, /* Handled by page cache/aops */
    .mmap       = generic_file_mmap,       /* Automatic memory mapping */
    .fsync      = veloxfs_fsync,           /* Force write to disk/journal */
};

static const struct inode_operations veloxfs_dir_inode_operations = {
    .lookup     = veloxfs_lookup,  /* Find files/folders in the table */
    .create     = veloxfs_create,  /* New file allocation */
    .mkdir      = veloxfs_mkdir,   /* Implicit directory creation */
    .unlink     = veloxfs_unlink,  /* File removal */
    .rmdir      = veloxfs_rmdir,   /* Directory removal */
};

static const struct file_operations veloxfs_dir_operations = {
    .iterate_shared = veloxfs_iterate, /* Your readdir implementation */
    .llseek         = generic_file_llseek,
};

/* 2. This fills the 'struct super_block' after the kernel mounts the device */
static int veloxfs_fill_super(struct super_block *sb, void *data, int silent) {
    struct buffer_head *bh;
    struct veloxfs_super *disk_super;
    struct inode *root;

    /* 1. Read the first block (Block 0) from the device */
    /* sb_bread returns a 'buffer_head' which points to the data in RAM */
    bh = sb_bread(sb, 0); 
    if (!bh) {
        printk(KERN_ERR "veloxfs: unable to read superblock\n");
        return -EIO;
    }

    disk_super = (struct veloxfs_super *)bh->b_data;

    /* 2. Verify the Magic Number (VLX6) */
    if (disk_super->magic != VELOXFS_MAGIC) {
        if (!silent) printk(KERN_ERR "veloxfs: not a v6 image (magic 0x%llx)\n", disk_super->magic);
        brelse(bh);
        return -EINVAL;
    }

    /* 3. Sync the kernel's block size with your formatted block size */
    if (!sb_set_blocksize(sb, disk_super->block_size)) {
        printk(KERN_ERR "veloxfs: device does not support block size %u\n", disk_super->block_size);
        brelse(bh);
        return -EINVAL;
    }

    /* Set internal kernel metadata */
    sb->s_magic = VELOXFS_MAGIC;
    sb->s_fs_info = disk_super; // Keep this handy for later

    /* 4. Root Inode Setup */
    root = new_inode(sb);
    root->i_ino = 1; // Conventional root inode number
    root->i_mode = S_IFDIR | 0755;
    root->i_op = &veloxfs_dir_inode_ops;
    root->i_fop = &veloxfs_dir_operations;

    sb->s_root = d_make_root(root);
    brelse(bh); // Release the buffer but the data is now in the superblock
    
    return 0;
}

/* 1. The Entry Point: Triggered by 'mount -t veloxfs' */
static struct dentry *veloxfs_mount(struct file_system_type *fs_type,
    int flags, const char *dev_name, void *data) {
    /* mount_bdev is for physical block devices (HDD/NAND) */
    return mount_bdev(fs_type, flags, dev_name, data, veloxfs_fill_super);
}

static struct file_system_type veloxfs_fs_type = {
    .owner = THIS_MODULE,
    .name = "veloxfs",
    .mount = veloxfs_mount,
    .kill_sb = kill_block_super,
    .fs_flags = FS_REQUIRES_DEV, // Tells Linux we need a disk/partition
};


/* Internal helper to find the physical block from the 4-extent map */
static sector_t veloxfs_find_phys_block(struct veloxfs_inode_info *vi, sector_t iblock) {
    uint64_t logical_offset = 0;
    int i;

    for (i = 0; i < 4; i++) {
        uint32_t count = vi->v_inode.extents[i].count;
        if (count == 0) break; // No more extents

        if (iblock < logical_offset + count) {
            /* The block is in this extent! */
            return vi->v_inode.extents[i].start + (iblock - logical_offset);
        }
        logical_offset += count;
    }

    /* Fallback: If not in the first 4 extents, you'd walk the FAT here */
    return veloxfs_walk_fat_chain(vi, iblock - logical_offset);
}

static int veloxfs_get_block(struct inode *inode, sector_t iblock, 
                             struct buffer_head *bh_result, int create) {
    struct veloxfs_inode_info *vi = VELOXFS_I(inode);
    sector_t phys_block;

    phys_block = veloxfs_find_phys_block(vi, iblock);
    
    if (phys_block) {
        /* Tell the kernel: logical block 'iblock' is at 'phys_block' on disk */
        map_bh(bh_result, inode->i_sb, phys_block);
    }
    
    return 0;
}

/* The actual "Read" entry point for the Page Cache */
static int veloxfs_read_folio(struct file *file, struct folio *folio) {
    return mpage_read_folio(folio, veloxfs_get_block);
}

/* Plug it into the VFS */
const struct address_space_operations veloxfs_aops = {
    .read_folio = veloxfs_read_folio,
    /* .write_folio and .direct_IO would go here later */
};

static sector_t veloxfs_walk_fat_chain(struct inode *inode, uint64_t relative_block) {
    struct super_block *sb = inode->i_sb;
    struct veloxfs_super *vs = sb->s_fs_info;
    struct veloxfs_inode_info *vi = VELOXFS_I(inode);
    
    uint64_t current_phys = vi->v_inode.fat_head;
    uint64_t i;
    
    /* 1. Sanity Check: If there's no FAT head, the file is just the extents */
    if (current_phys == 0 || current_phys == 0xFFFFFFFFFFFFFFFF)
        return 0;

    /* 2. Walk the chain relative_block times */
    for (i = 0; i < relative_block; i++) {
        struct buffer_head *bh;
        uint64_t *fat_page;
        uint64_t entries_per_block = sb->s_blocksize / sizeof(uint64_t);
        
        /* Which block of the FAT contains entry 'current_phys'? */
        sector_t fat_block_idx = vs->fat_start + (current_phys / entries_per_block);
        uint32_t offset_in_block = current_phys % entries_per_block;

        bh = sb_bread(sb, fat_block_idx);
        if (!bh) return 0;

        fat_page = (uint64_t *)bh->b_data;
        current_phys = fat_page[offset_in_block];
        
        brelse(bh); // CRITICAL: Release the buffer after reading the pointer

        /* Check for end of file or corruption */
        if (current_phys == 0xFFFFFFFFFFFFFFFF || current_phys == 0)
            return 0;
            
        /* Safety: prevent infinite loops from circular FAT chains */
        if (i > vs->block_count) return 0; 
    }

    return current_phys;
}

static struct dentry *veloxfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags) {
    struct super_block *sb = dir->i_sb;
    struct veloxfs_inode disk_ino;
    struct inode *inode = NULL;
    int ino_idx;

    /* 1. Use your internal hash/table logic to find the inode index by name */
    /* dentry->d_name.name is the string "my_file.txt" */
    ino_idx = veloxfs_find_entry(sb, dentry->d_name.name);

    if (ino_idx >= 0) {
        /* 2. Read the 144-byte metadata from the disk */
        if (veloxfs_read_disk_inode(sb, ino_idx, &disk_ino) == 0) {
            
            /* 3. Create a new kernel inode and "hydrate" it */
            inode = new_inode(sb);
            if (inode) {
                inode->i_ino = ino_idx;
                veloxfs_fill_inode(inode, &disk_ino);
            }
        }
    }

    /* 4. d_add links the name to the inode (or NULL if not found) */
    return d_splice_alias(inode, dentry);
}

static struct inode *veloxfs_new_inode(struct inode *dir, umode_t mode) {
    struct super_block *sb = dir->i_sb;
    struct inode *inode;
    int ino_idx;

    /* 1. Find a free bit in your inode bitmap/table */
    ino_idx = veloxfs_find_free_inode_idx(sb); 
    if (ino_idx < 0) return ERR_PTR(-ENOSPC);

    inode = new_inode(sb);
    if (!inode) return ERR_PTR(-ENOMEM);

    /* 2. Fill standard VFS metadata */
    inode_init_owner(&init_user_ns, inode, dir, mode);
    inode->i_ino = ino_idx;
    inode->i_blocks = 0;
    inode->i_mtime = inode->i_atime = inode->i_ctime = current_time(inode);
    
    /* 3. Set the operations based on file vs directory */
    if (S_ISDIR(mode)) {
        inode->i_op = &veloxfs_dir_inode_operations;
        inode->i_fop = &veloxfs_dir_operations;
    } else {
        inode->i_op = &veloxfs_file_inode_operations;
        inode->i_fop = &veloxfs_file_operations;
        inode->i_mapping->a_ops = &veloxfs_aops; // Your 4-extent logic
    }

    insert_inode_hash(inode); // Make it findable in the kernel cache
    mark_inode_dirty(inode);  // Tell the kernel it needs to be written to disk
    
    return inode;
}

/* Standard File Creation */
static int veloxfs_create(struct user_namespace *mnt_userns, struct inode *dir,
                         struct dentry *dentry, umode_t mode, bool excl) {
    struct inode *inode = veloxfs_new_inode(dir, mode);
    struct veloxfs_inode_info *vi = VELOXFS_I(inode);
    uint64_t start;
    int err;

    /* Pre-allocate one block for the new file to reduce fragmentation later */
    err = veloxfs_allocate_blocks(inode->i_sb, 1, &start);
    if (err == 0) {
        vi->v_inode.extents[0].start = start;
        vi->v_inode.extents[0].count = 1;
        inode->i_blocks = 1;
    }

    d_instantiate(dentry, inode);
    return 0;
}

/* Directory Creation (The "Implicit" Hack) */
static int veloxfs_mkdir(struct user_namespace *mnt_userns, struct inode *dir,
                        struct dentry *dentry, umode_t mode) {
    int err;
    char path_buf[NAME_MAX];

    /* 1. Create the 'directory' inode in the kernel's eyes */
    err = veloxfs_create(mnt_userns, dir, dentry, S_IFDIR | mode, false);
    if (err) return err;

    /* 2. Logic Hack: Automatically create the .directoryfile */
    /* This ensures that if the system reboots, VeloxFS sees this 
       path as a valid directory because it contains a file. */
    snprintf(path_buf, NAME_MAX, "%s/.directoryfile", dentry->d_name.name);
    return veloxfs_internal_create_hidden_file(dentry->d_inode, path_buf);
}

static int veloxfs_write_inode(struct inode *inode, struct writeback_control *wbc) {
    struct buffer_head *bh;
    struct veloxfs_inode *disk_ino;
    struct veloxfs_inode_info *vi = VELOXFS_I(inode);
    
    /* 1. Calculate which block and offset holds this inode index */
    sector_t block = calculate_inode_block(inode->i_sb, inode->i_ino);
    int offset = calculate_inode_offset(inode->i_sb, inode->i_ino);

    bh = sb_bread(inode->i_sb, block);
    if (!bh) return -EIO;

    /* 2. Map kernel inode fields back to your 144-byte struct */
    disk_ino = (struct veloxfs_inode *)(bh->b_data + offset);
    disk_ino->size = inode->i_size;
    disk_ino->mode = inode->i_mode;
    // Copy your 4-extents from vi->v_inode...

    /* 3. Commit to disk */
    mark_buffer_dirty(bh);
    if (wbc->sync_mode == WB_SYNC_ALL) sync_dirty_buffer(bh);
    brelse(bh);

    return 0;
}

/* Internal helper to find a contiguous run of N blocks */
static int veloxfs_find_free_run(struct super_block *sb, uint32_t needed, uint64_t *start_block) {
    struct veloxfs_super *vs = sb->s_fs_info;
    struct buffer_head *bh;
    uint32_t bitmap_block = vs->bitmap_start;
    uint64_t current_bit = 0;
    uint32_t found_run = 0;

    /* Iterate through bitmap blocks */
    for (int i = 0; i < vs->bitmap_block_count; i++) {
        bh = sb_bread(sb, bitmap_block + i);
        if (!bh) return -EIO;

        /* Scan bits in this block (e.g., 64KB block = 524,288 bits) */
        unsigned long *bitmap = (unsigned long *)bh->b_data;
        int bit = find_next_zero_bit(bitmap, sb->s_blocksize * 8, 0);
        
        // ... (Logic to verify if 'needed' consecutive bits are zero)
        
        brelse(bh);
        if (found) {
            *start_block = calculated_index;
            return 0;
        }
    }
    return -ENOSPC;
}

static void veloxfs_free_blocks(struct super_block *sb, uint64_t start_block, uint32_t count) {
    struct veloxfs_super *vs = sb->s_fs_info;
    struct buffer_head *bh;
    uint64_t current_block = start_block;
    uint32_t remaining = count;

    /* Lock the bitmap to prevent race conditions during deallocation */
    mutex_lock(&vs->bitmap_lock);

    while (remaining > 0) {
        uint32_t bitmap_block_idx = vs->bitmap_start + (current_block / (sb->s_blocksize * 8));
        uint32_t bit_offset = current_block % (sb->s_blocksize * 8);
        uint32_t blocks_in_this_bh = min(remaining, (uint32_t)(sb->s_blocksize * 8) - bit_offset);

        bh = sb_bread(sb, bitmap_block_idx);
        if (!bh) {
            pr_err("veloxfs: critical error reading bitmap block %u\n", bitmap_block_idx);
            break; 
        }

        /* Atomically clear the bits */
        for (uint32_t i = 0; i < blocks_in_this_bh; i++) {
            clear_bit(bit_offset + i, (unsigned long *)bh->b_data);
        }

        mark_buffer_dirty(bh);
        brelse(bh);

        remaining -= blocks_in_this_bh;
        current_block += blocks_in_this_bh;
    }

    mutex_unlock(&vs->bitmap_lock);
}

static int veloxfs_setattr(struct user_namespace *mnt_userns, struct dentry *dentry,
                          struct iattr *attr) {
    struct inode *inode = d_inode(dentry);
    int error;

    /* 1. Standard VFS checks (ownership, permissions) */
    error = setattr_prepare(mnt_userns, dentry, attr);
    if (error) return error;

    /* 2. Check if this is a size change */
    if (attr->ia_valid & ATTR_SIZE) {
        loff_t old_size = inode->i_size;
        loff_t new_size = attr->ia_size;

        if (new_size < old_size) {
            /* Shrinking: We need to free blocks */
            error = veloxfs_truncate_blocks(inode, new_size);
            if (error) return error;
        }
        
        /* Note: If new_size > old_size, Linux handles the "hole" 
           automatically via the page cache. */
        truncate_setsize(inode, new_size);
    }

    /* 3. Commit remaining metadata changes to the VFS inode */
    setattr_copy(mnt_userns, inode, attr);
    mark_inode_dirty(inode);
    return 0;
}

static int veloxfs_truncate_fat(struct inode *inode, uint64_t keep_until_logical) {
    struct veloxfs_inode_info *vi = VELOXFS_I(inode);
    struct super_block *sb = inode->i_sb;
    uint64_t current_phys = vi->v_inode.fat_head;
    
    /* 1. Walk to the new tail of the file */
    for (uint64_t i = 0; i < keep_until_logical; i++) {
        current_phys = veloxfs_get_next_fat_entry(sb, current_phys);
    }

    /* 2. Save the pointer to the first block to be deleted */
    uint64_t to_delete = veloxfs_get_next_fat_entry(sb, current_phys);

    /* 3. Terminate the chain at the new tail */
    veloxfs_set_fat_entry(sb, current_phys, 0xFFFFFFFFFFFFFFFF);

    /* 4. Recursively free everything from 'to_delete' onwards */
    while (to_delete != 0xFFFFFFFFFFFFFFFF) {
        uint64_t next = veloxfs_get_next_fat_entry(sb, to_delete);
        veloxfs_free_blocks(sb, to_delete, 1);
        to_delete = next;
    }
    
    return 0;
}

static void veloxfs_evict_inode(struct inode *inode) {
    struct veloxfs_inode_info *vi = VELOXFS_I(inode);
    struct super_block *sb = inode->i_sb;

    /* 1. Clear the kernel's internal page cache for this inode */
    truncate_inode_pages_final(&inode->i_data);

    /* 2. Check if the file was actually deleted (unlinked) */
    if (inode->i_nlink == 0) {
        /* Set size to 0 to trigger our truncation/reclamation logic */
        inode->i_size = 0;
        
        /* This helper (which we'll define below) frees extents + FAT chain */
        veloxfs_truncate_blocks(inode, 0);

        /* 3. Mark the inode slot as free in the inode bitmap */
        veloxfs_free_inode_slot(sb, inode->i_ino);
    }

    /* 4. Mandatory VFS cleanup */
    invalidate_inode_buffers(inode);
    clear_inode(inode);

    /* 5. Clean up your private memory */
    // Note: The actual 'vi' struct is usually freed by your 
    // super_block's alloc_inode/free_inode callbacks.
}

static int veloxfs_journal_commit(struct super_block *sb, struct veloxfs_journal_entry *entry) {
    struct buffer_head *bh;
    sector_t journal_block = veloxfs_get_next_journal_slot(sb);

    /* 1. Get the journal block */
    bh = sb_getblk(sb, journal_block);
    if (!bh) return -ENOMEM;

    /* 2. Copy the transaction data */
    memcpy(bh->b_data, entry, sizeof(*entry));
    set_buffer_uptodate(bh);
    mark_buffer_dirty(bh);

    /* 3. FORCE the write to physical media */
    sync_dirty_buffer(bh); 
    
    /* 4. The Hardware Barrier */
    /* This ensures the journal is ON THE DISK before we touch the bitmap */
    blkdev_issue_flush(sb->s_bdev, GFP_KERNEL, NULL);

    brelse(bh);
    return 0;
}

static int veloxfs_iterate(struct file *file, struct dir_context *ctx) {
    struct inode *inode = file_inode(file);
    struct super_block *sb = inode->i_sb;
    struct veloxfs_super *vs = sb->s_fs_info;
    int i;

    /* 1. Handle the standard "." and ".." entries first */
    if (!dir_emit_dots(file, ctx))
        return 0;

    /* 2. Start scanning from the last saved position (ctx->pos) */
    /* We skip the first 2 positions because dir_emit_dots handled them */
    for (i = ctx->pos - 2; i < vs->total_inodes; i++) {
        struct veloxfs_inode disk_ino;
        char filename[NAME_MAX];

        /* Read the metadata for this index */
        if (veloxfs_get_inode_by_idx(sb, i, &disk_ino, filename) != 0)
            continue;

        /* 3. THE FILTER: Hide the .directoryfile anchor */
        if (strcmp(filename, ".directoryfile") == 0) {
            ctx->pos++;
            continue;
        }

        /* 4. Emit the entry to the VFS */
        /* 'dir_emit' returns false if the user's buffer is full */
        if (!dir_emit(ctx, filename, strlen(filename), i, DT_REG))
            return 0;

        ctx->pos++;
    }

    return 0;
}

int veloxfs_verify_inode(struct veloxfs_inode *ino) {
    /* Calculate CRC32 of the first 140 bytes */
    uint32_t calced = crc32_le(0xFFFFFFFF, (unsigned char *)ino, 140);
    
    if (calced != ino->i_crc) {
        pr_crit("veloxfs: Checksum mismatch on inode! Potential bit-rot.\n");
        return -EIO;
    }
    return 0;
}

/* When writing the inode back to disk */
void veloxfs_set_inode_checksum(struct veloxfs_inode *ino) {
    ino->i_crc = crc32_le(0xFFFFFFFF, (unsigned char *)ino, 140);
}

static int veloxfs_setattr(struct user_namespace *mnt_userns, struct dentry *dentry,
                          struct iattr *attr) {
    struct inode *inode = d_inode(dentry);
    int error;

    /* 1. Preliminary VFS permission and ownership checks */
    error = setattr_prepare(mnt_userns, dentry, attr);
    if (error) return error;

    /* 2. Handle Size Change (Truncate) */
    if (attr->ia_valid & ATTR_SIZE) {
        loff_t old_size = inode->i_size;
        loff_t new_size = attr->ia_size;

        if (new_size < old_size) {
            /* Shrinking: Reclaim physical blocks */
            error = veloxfs_truncate_blocks(inode, new_size);
            if (error) return error;
        }
        
        /* Update page cache and VFS size */
        truncate_setsize(inode, new_size);
    }

    /* 3. Handle other attributes (mtime, permissions) */
    setattr_copy(mnt_userns, inode, attr);
    mark_inode_dirty(inode);
    return 0;
}


static void veloxfs_init_once(void *foo) {
    struct veloxfs_inode_info *vi = (struct veloxfs_inode_info *)foo;
    inode_init_once(&vi->vfs_inode);
}

static int __init init_veloxfs_fs(void) {
    int err;

    /* 1. Create the Slab Cache for our custom inode wrapper */
    veloxfs_inode_cachep = kmem_cache_create("veloxfs_inode_cache",
                                            sizeof(struct veloxfs_inode_info),
                                            0, (SLAB_RECLAIM_ACCOUNT|SLAB_MEM_SPREAD),
                                            veloxfs_init_once);
    if (!veloxfs_inode_cachep)
        return -ENOMEM;

    /* 2. Register the filesystem with the VFS */
    err = register_filesystem(&veloxfs_fs_type);
    if (err) {
        kmem_cache_destroy(veloxfs_inode_cachep);
        return err;
    }

    pr_info("VeloxFS v6: Storage-Optimized Driver Loaded\n");
    return 0;
}

static void __exit exit_veloxfs_fs(void) {
    /* Order is reversed: unregister first, then destroy memory pools */
    unregister_filesystem(&veloxfs_fs_type);
    
    /* Ensure all RCU callbacks are finished before destroying the cache */
    rcu_barrier();
    kmem_cache_destroy(veloxfs_inode_cachep);
    
    pr_info("VeloxFS v6: Driver Unloaded\n");
}



module_init(init_veloxfs_fs);
module_exit(exit_veloxfs_fs);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Maxwell Wingate");
MODULE_DESCRIPTION("VeloxFS v6 Kernel Driver");