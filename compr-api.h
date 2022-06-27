#ifndef COMPR_API_H
#define COMPR_API_H

struct page;


/* compress() compresses the page in src into dest. Returns 0 if compression to
 * the huge page threshold MAX_NON_HUGE failed, otherwise the size of the
 * compressed data in dest.
 */
size_type compress(char src[static PAGE_SIZE],
				 char dest[static MAX_NON_HUGE]);

/* decompress() decompresses 'bytes' bytes from pg->data into dest. Returns a
 * negative error code if pg->data isn't formatted as a compressed page,
 * otherwise 0.
 */
int decompress(struct page *pg, size_type bytes,
			     char dest[static PAGE_SIZE]);
