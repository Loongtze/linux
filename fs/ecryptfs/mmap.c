// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * eCryptfs: Linux filesystem encryption layer
 * This is where eCryptfs coordinates the symmetric encryption and
 * decryption of the file data as it passes between the lower
 * encrypted file and the upper decrypted file.
 *
 * Copyright (C) 1997-2003 Erez Zadok
 * Copyright (C) 2001-2003 Stony Brook University
 * Copyright (C) 2004-2007 International Business Machines Corp.
 *   Author(s): Michael A. Halcrow <mahalcro@us.ibm.com>
 */

#include <linux/pagemap.h>
#include <linux/writeback.h>
#include <linux/page-flags.h>
#include <linux/mount.h>
#include <linux/file.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/xattr.h>
#include <linux/unaligned.h>
#include "ecryptfs_kernel.h"

/*
 * This is where we encrypt the data and pass the encrypted data to
 * the lower filesystem.  In OpenPGP-compatible mode, we operate on
 * entire underlying packets.
 */
static int ecryptfs_writepages(struct address_space *mapping,
		struct writeback_control *wbc)
{
	struct folio *folio = NULL;
	int error;

	while ((folio = writeback_iter(mapping, wbc, folio, &error))) {
		error = ecryptfs_encrypt_page(folio);
		if (error) {
			ecryptfs_printk(KERN_WARNING,
				"Error encrypting folio (index [0x%.16lx])\n",
				folio->index);
			folio_clear_uptodate(folio);
			mapping_set_error(mapping, error);
		}
		folio_unlock(folio);
	}

	return error;
}

static void strip_xattr_flag(char *page_virt,
			     struct ecryptfs_crypt_stat *crypt_stat)
{
	if (crypt_stat->flags & ECRYPTFS_METADATA_IN_XATTR) {
		size_t written;

		crypt_stat->flags &= ~ECRYPTFS_METADATA_IN_XATTR;
		ecryptfs_write_crypt_stat_flags(page_virt, crypt_stat,
						&written);
		crypt_stat->flags |= ECRYPTFS_METADATA_IN_XATTR;
	}
}

/*
 *   Header Extent:
 *     Octets 0-7:        Unencrypted file size (big-endian)
 *     Octets 8-15:       eCryptfs special marker
 *     Octets 16-19:      Flags
 *      Octet 16:         File format version number (between 0 and 255)
 *      Octets 17-18:     Reserved
 *      Octet 19:         Bit 1 (lsb): Reserved
 *                        Bit 2: Encrypted?
 *                        Bits 3-8: Reserved
 *     Octets 20-23:      Header extent size (big-endian)
 *     Octets 24-25:      Number of header extents at front of file
 *                        (big-endian)
 *     Octet  26:         Begin RFC 2440 authentication token packet set
 */

/**
 * ecryptfs_copy_up_encrypted_with_header
 * @folio: Sort of a ``virtual'' representation of the encrypted lower
 *        file. The actual lower file does not have the metadata in
 *        the header. This is locked.
 * @crypt_stat: The eCryptfs inode's cryptographic context
 *
 * The ``view'' is the version of the file that userspace winds up
 * seeing, with the header information inserted.
 */
static int
ecryptfs_copy_up_encrypted_with_header(struct folio *folio,
				       struct ecryptfs_crypt_stat *crypt_stat)
{
	loff_t extent_num_in_page = 0;
	loff_t num_extents_per_page = (PAGE_SIZE
				       / crypt_stat->extent_size);
	int rc = 0;

	while (extent_num_in_page < num_extents_per_page) {
		loff_t view_extent_num = ((loff_t)folio->index
					   * num_extents_per_page)
					  + extent_num_in_page;
		size_t num_header_extents_at_front =
			(crypt_stat->metadata_size / crypt_stat->extent_size);

		if (view_extent_num < num_header_extents_at_front) {
			/* This is a header extent */
			char *page_virt;

			page_virt = kmap_local_folio(folio, 0);
			memset(page_virt, 0, PAGE_SIZE);
			/* TODO: Support more than one header extent */
			if (view_extent_num == 0) {
				size_t written;

				rc = ecryptfs_read_xattr_region(
					page_virt, folio->mapping->host);
				strip_xattr_flag(page_virt + 16, crypt_stat);
				ecryptfs_write_header_metadata(page_virt + 20,
							       crypt_stat,
							       &written);
			}
			kunmap_local(page_virt);
			flush_dcache_folio(folio);
			if (rc) {
				printk(KERN_ERR "%s: Error reading xattr "
				       "region; rc = [%d]\n", __func__, rc);
				goto out;
			}
		} else {
			/* This is an encrypted data extent */
			loff_t lower_offset =
				((view_extent_num * crypt_stat->extent_size)
				 - crypt_stat->metadata_size);

			rc = ecryptfs_read_lower_page_segment(
				folio, (lower_offset >> PAGE_SHIFT),
				(lower_offset & ~PAGE_MASK),
				crypt_stat->extent_size, folio->mapping->host);
			if (rc) {
				printk(KERN_ERR "%s: Error attempting to read "
				       "extent at offset [%lld] in the lower "
				       "file; rc = [%d]\n", __func__,
				       lower_offset, rc);
				goto out;
			}
		}
		extent_num_in_page++;
	}
out:
	return rc;
}

/**
 * ecryptfs_read_folio
 * @file: An eCryptfs file
 * @folio: Folio from eCryptfs inode mapping into which to stick the read data
 *
 * Read in a folio, decrypting if necessary.
 *
 * Returns zero on success; non-zero on error.
 */
static int ecryptfs_read_folio(struct file *file, struct folio *folio)
{
	struct inode *inode = folio->mapping->host;
	struct ecryptfs_crypt_stat *crypt_stat =
		&ecryptfs_inode_to_private(inode)->crypt_stat;
	int err = 0;

	if (!crypt_stat || !(crypt_stat->flags & ECRYPTFS_ENCRYPTED)) {
		err = ecryptfs_read_lower_page_segment(folio, folio->index, 0,
				folio_size(folio), inode);
	} else if (crypt_stat->flags & ECRYPTFS_VIEW_AS_ENCRYPTED) {
		if (crypt_stat->flags & ECRYPTFS_METADATA_IN_XATTR) {
			err = ecryptfs_copy_up_encrypted_with_header(folio,
					crypt_stat);
			if (err) {
				printk(KERN_ERR "%s: Error attempting to copy "
				       "the encrypted content from the lower "
				       "file whilst inserting the metadata "
				       "from the xattr into the header; err = "
				       "[%d]\n", __func__, err);
				goto out;
			}

		} else {
			err = ecryptfs_read_lower_page_segment(folio,
					folio->index, 0, folio_size(folio),
					inode);
			if (err) {
				printk(KERN_ERR "Error reading page; err = "
				       "[%d]\n", err);
				goto out;
			}
		}
	} else {
		err = ecryptfs_decrypt_page(folio);
		if (err) {
			ecryptfs_printk(KERN_ERR, "Error decrypting page; "
					"err = [%d]\n", err);
			goto out;
		}
	}
out:
	ecryptfs_printk(KERN_DEBUG, "Unlocking folio with index = [0x%.16lx]\n",
			folio->index);
	folio_end_read(folio, err == 0);
	return err;
}

/*
 * Called with lower inode mutex held.
 */
static int fill_zeros_to_end_of_page(struct folio *folio, unsigned int to)
{
	struct inode *inode = folio->mapping->host;
	int end_byte_in_page;

	if ((i_size_read(inode) / PAGE_SIZE) != folio->index)
		goto out;
	end_byte_in_page = i_size_read(inode) % PAGE_SIZE;
	if (to > end_byte_in_page)
		end_byte_in_page = to;
	folio_zero_segment(folio, end_byte_in_page, PAGE_SIZE);
out:
	return 0;
}

/**
 * ecryptfs_write_begin
 * @iocb: I/O control block for the eCryptfs file
 * @mapping: The eCryptfs object
 * @pos: The file offset at which to start writing
 * @len: Length of the write
 * @foliop: Pointer to return the folio
 * @fsdata: Pointer to return fs data (unused)
 *
 * This function must zero any hole we create
 *
 * Returns zero on success; non-zero otherwise
 */
static int ecryptfs_write_begin(const struct kiocb *iocb,
			struct address_space *mapping,
			loff_t pos, unsigned len,
			struct folio **foliop, void **fsdata)
{
	pgoff_t index = pos >> PAGE_SHIFT;
	struct folio *folio;
	loff_t prev_page_end_size;
	int rc = 0;

	folio = __filemap_get_folio(mapping, index, FGP_WRITEBEGIN,
			mapping_gfp_mask(mapping));
	if (IS_ERR(folio))
		return PTR_ERR(folio);
	*foliop = folio;

	prev_page_end_size = ((loff_t)index << PAGE_SHIFT);
	if (!folio_test_uptodate(folio)) {
		struct ecryptfs_crypt_stat *crypt_stat =
			&ecryptfs_inode_to_private(mapping->host)->crypt_stat;

		if (!(crypt_stat->flags & ECRYPTFS_ENCRYPTED)) {
			rc = ecryptfs_read_lower_page_segment(
				folio, index, 0, PAGE_SIZE, mapping->host);
			if (rc) {
				printk(KERN_ERR "%s: Error attempting to read "
				       "lower page segment; rc = [%d]\n",
				       __func__, rc);
				folio_clear_uptodate(folio);
				goto out;
			} else
				folio_mark_uptodate(folio);
		} else if (crypt_stat->flags & ECRYPTFS_VIEW_AS_ENCRYPTED) {
			if (crypt_stat->flags & ECRYPTFS_METADATA_IN_XATTR) {
				rc = ecryptfs_copy_up_encrypted_with_header(
					folio, crypt_stat);
				if (rc) {
					printk(KERN_ERR "%s: Error attempting "
					       "to copy the encrypted content "
					       "from the lower file whilst "
					       "inserting the metadata from "
					       "the xattr into the header; rc "
					       "= [%d]\n", __func__, rc);
					folio_clear_uptodate(folio);
					goto out;
				}
				folio_mark_uptodate(folio);
			} else {
				rc = ecryptfs_read_lower_page_segment(
					folio, index, 0, PAGE_SIZE,
					mapping->host);
				if (rc) {
					printk(KERN_ERR "%s: Error reading "
					       "page; rc = [%d]\n",
					       __func__, rc);
					folio_clear_uptodate(folio);
					goto out;
				}
				folio_mark_uptodate(folio);
			}
		} else {
			if (prev_page_end_size
			    >= i_size_read(mapping->host)) {
				folio_zero_range(folio, 0, PAGE_SIZE);
				folio_mark_uptodate(folio);
			} else if (len < PAGE_SIZE) {
				rc = ecryptfs_decrypt_page(folio);
				if (rc) {
					printk(KERN_ERR "%s: Error decrypting "
					       "page at index [%ld]; "
					       "rc = [%d]\n",
					       __func__, folio->index, rc);
					folio_clear_uptodate(folio);
					goto out;
				}
				folio_mark_uptodate(folio);
			}
		}
	}
	/* If creating a page or more of holes, zero them out via truncate.
	 * Note, this will increase i_size. */
	if (index != 0) {
		if (prev_page_end_size > i_size_read(mapping->host)) {
			rc = ecryptfs_truncate(iocb->ki_filp->f_path.dentry,
					       prev_page_end_size);
			if (rc) {
				printk(KERN_ERR "%s: Error on attempt to "
				       "truncate to (higher) offset [%lld];"
				       " rc = [%d]\n", __func__,
				       prev_page_end_size, rc);
				goto out;
			}
		}
	}
	/* Writing to a new page, and creating a small hole from start
	 * of page?  Zero it out. */
	if ((i_size_read(mapping->host) == prev_page_end_size)
	    && (pos != 0))
		folio_zero_range(folio, 0, PAGE_SIZE);
out:
	if (unlikely(rc)) {
		folio_unlock(folio);
		folio_put(folio);
	}
	return rc;
}

/*
 * ecryptfs_write_inode_size_to_header
 *
 * Writes the lower file size to the first 8 bytes of the header.
 *
 * Returns zero on success; non-zero on error.
 */
static int ecryptfs_write_inode_size_to_header(struct inode *ecryptfs_inode)
{
	char *file_size_virt;
	int rc;

	file_size_virt = kmalloc(sizeof(u64), GFP_KERNEL);
	if (!file_size_virt) {
		rc = -ENOMEM;
		goto out;
	}
	put_unaligned_be64(i_size_read(ecryptfs_inode), file_size_virt);
	rc = ecryptfs_write_lower(ecryptfs_inode, file_size_virt, 0,
				  sizeof(u64));
	kfree(file_size_virt);
	if (rc < 0)
		printk(KERN_ERR "%s: Error writing file size to header; "
		       "rc = [%d]\n", __func__, rc);
	else
		rc = 0;
out:
	return rc;
}

struct kmem_cache *ecryptfs_xattr_cache;

static int ecryptfs_write_inode_size_to_xattr(struct inode *ecryptfs_inode)
{
	ssize_t size;
	void *xattr_virt;
	struct dentry *lower_dentry =
		ecryptfs_inode_to_private(ecryptfs_inode)->lower_file->f_path.dentry;
	struct inode *lower_inode = d_inode(lower_dentry);
	int rc;

	if (!(lower_inode->i_opflags & IOP_XATTR)) {
		printk(KERN_WARNING
		       "No support for setting xattr in lower filesystem\n");
		rc = -ENOSYS;
		goto out;
	}
	xattr_virt = kmem_cache_alloc(ecryptfs_xattr_cache, GFP_KERNEL);
	if (!xattr_virt) {
		rc = -ENOMEM;
		goto out;
	}
	inode_lock(lower_inode);
	size = __vfs_getxattr(lower_dentry, lower_inode, ECRYPTFS_XATTR_NAME,
			      xattr_virt, PAGE_SIZE);
	if (size < 0)
		size = 8;
	put_unaligned_be64(i_size_read(ecryptfs_inode), xattr_virt);
	rc = __vfs_setxattr(&nop_mnt_idmap, lower_dentry, lower_inode,
			    ECRYPTFS_XATTR_NAME, xattr_virt, size, 0);
	inode_unlock(lower_inode);
	if (rc)
		printk(KERN_ERR "Error whilst attempting to write inode size "
		       "to lower file xattr; rc = [%d]\n", rc);
	kmem_cache_free(ecryptfs_xattr_cache, xattr_virt);
out:
	return rc;
}

int ecryptfs_write_inode_size_to_metadata(struct inode *ecryptfs_inode)
{
	struct ecryptfs_crypt_stat *crypt_stat;

	crypt_stat = &ecryptfs_inode_to_private(ecryptfs_inode)->crypt_stat;
	BUG_ON(!(crypt_stat->flags & ECRYPTFS_ENCRYPTED));
	if (crypt_stat->flags & ECRYPTFS_METADATA_IN_XATTR)
		return ecryptfs_write_inode_size_to_xattr(ecryptfs_inode);
	else
		return ecryptfs_write_inode_size_to_header(ecryptfs_inode);
}

/**
 * ecryptfs_write_end
 * @iocb: I/O control block for the eCryptfs file
 * @mapping: The eCryptfs object
 * @pos: The file position
 * @len: The length of the data (unused)
 * @copied: The amount of data copied
 * @folio: The eCryptfs folio
 * @fsdata: The fsdata (unused)
 */
static int ecryptfs_write_end(const struct kiocb *iocb,
			struct address_space *mapping,
			loff_t pos, unsigned len, unsigned copied,
			struct folio *folio, void *fsdata)
{
	pgoff_t index = pos >> PAGE_SHIFT;
	unsigned from = pos & (PAGE_SIZE - 1);
	unsigned to = from + copied;
	struct inode *ecryptfs_inode = mapping->host;
	struct ecryptfs_crypt_stat *crypt_stat =
		&ecryptfs_inode_to_private(ecryptfs_inode)->crypt_stat;
	int rc;

	ecryptfs_printk(KERN_DEBUG, "Calling fill_zeros_to_end_of_page"
			"(page w/ index = [0x%.16lx], to = [%d])\n", index, to);
	if (!(crypt_stat->flags & ECRYPTFS_ENCRYPTED)) {
		rc = ecryptfs_write_lower_page_segment(ecryptfs_inode,
				folio, 0, to);
		if (!rc) {
			rc = copied;
			fsstack_copy_inode_size(ecryptfs_inode,
				ecryptfs_inode_to_lower(ecryptfs_inode));
		}
		goto out;
	}
	if (!folio_test_uptodate(folio)) {
		if (copied < PAGE_SIZE) {
			rc = 0;
			goto out;
		}
		folio_mark_uptodate(folio);
	}
	/* Fills in zeros if 'to' goes beyond inode size */
	rc = fill_zeros_to_end_of_page(folio, to);
	if (rc) {
		ecryptfs_printk(KERN_WARNING, "Error attempting to fill "
			"zeros in page with index = [0x%.16lx]\n", index);
		goto out;
	}
	rc = ecryptfs_encrypt_page(folio);
	if (rc) {
		ecryptfs_printk(KERN_WARNING, "Error encrypting page (upper "
				"index [0x%.16lx])\n", index);
		goto out;
	}
	if (pos + copied > i_size_read(ecryptfs_inode)) {
		i_size_write(ecryptfs_inode, pos + copied);
		ecryptfs_printk(KERN_DEBUG, "Expanded file size to "
			"[0x%.16llx]\n",
			(unsigned long long)i_size_read(ecryptfs_inode));
	}
	rc = ecryptfs_write_inode_size_to_metadata(ecryptfs_inode);
	if (rc)
		printk(KERN_ERR "Error writing inode size to metadata; "
		       "rc = [%d]\n", rc);
	else
		rc = copied;
out:
	folio_unlock(folio);
	folio_put(folio);
	return rc;
}

static sector_t ecryptfs_bmap(struct address_space *mapping, sector_t block)
{
	struct inode *lower_inode = ecryptfs_inode_to_lower(mapping->host);
	int ret = bmap(lower_inode, &block);

	if (ret)
		return 0;
	return block;
}

#include <linux/buffer_head.h>

const struct address_space_operations ecryptfs_aops = {
	/*
	 * XXX: This is pretty broken for multiple reasons: ecryptfs does not
	 * actually use buffer_heads, and ecryptfs will crash without
	 * CONFIG_BLOCK.  But it matches the behavior before the default for
	 * address_space_operations without the ->dirty_folio method was
	 * cleaned up, so this is the best we can do without maintainer
	 * feedback.
	 */
#ifdef CONFIG_BLOCK
	.dirty_folio	= block_dirty_folio,
	.invalidate_folio = block_invalidate_folio,
#endif
	.writepages = ecryptfs_writepages,
	.read_folio = ecryptfs_read_folio,
	.write_begin = ecryptfs_write_begin,
	.write_end = ecryptfs_write_end,
	.migrate_folio = filemap_migrate_folio,
	.bmap = ecryptfs_bmap,
};
