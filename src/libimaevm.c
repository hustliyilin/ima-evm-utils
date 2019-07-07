/*
 * ima-evm-utils - IMA/EVM support utilities
 *
 * Copyright (C) 2011 Nokia Corporation
 * Copyright (C) 2011,2012,2013 Intel Corporation
 * Copyright (C) 2013,2014 Samsung Electronics
 *
 * Authors:
 * Dmitry Kasatkin <dmitry.kasatkin@nokia.com>
 *                 <dmitry.kasatkin@intel.com>
 *                 <d.kasatkin@samsung.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 *
 * File: libimaevm.c
 *	 IMA/EVM library
 */

/* should we use logger instead for library? */
#define USE_FPRINTF

#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <asm/byteorder.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <ctype.h>

#include <openssl/crypto.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/err.h>

#include "imaevm.h"
#include "hash_info.h"

/* Names that are primary for OpenSSL. */
const char *const pkey_hash_algo[PKEY_HASH__LAST] = {
	[PKEY_HASH_MD4]		= "md4",
	[PKEY_HASH_MD5]		= "md5",
	[PKEY_HASH_SHA1]	= "sha1",
	[PKEY_HASH_RIPE_MD_160]	= "rmd160",
	[PKEY_HASH_SHA256]	= "sha256",
	[PKEY_HASH_SHA384]	= "sha384",
	[PKEY_HASH_SHA512]	= "sha512",
	[PKEY_HASH_SHA224]	= "sha224",
	[PKEY_HASH_STREEBOG_256] = "md_gost12_256",
	[PKEY_HASH_STREEBOG_512] = "md_gost12_512",
};

/* Names that are primary for the kernel. */
const char *const pkey_hash_algo_kern[PKEY_HASH__LAST] = {
	[PKEY_HASH_STREEBOG_256] = "streebog256",
	[PKEY_HASH_STREEBOG_512] = "streebog512",
};

struct libevm_params params = {
	.verbose = LOG_INFO - 1,
	.x509 = 1,
	.hash_algo = "sha1",
};

static void __attribute__ ((constructor)) libinit(void);

void do_dump(FILE *fp, const void *ptr, int len, bool cr)
{
	int i;
	uint8_t *data = (uint8_t *) ptr;

	for (i = 0; i < len; i++)
		fprintf(fp, "%02x", data[i]);
	if (cr)
		fprintf(fp, "\n");
}

void dump(const void *ptr, int len)
{
	do_dump(stdout, ptr, len, true);
}

const char *get_hash_algo_by_id(int algo)
{
	if (algo < PKEY_HASH__LAST)
		return pkey_hash_algo[algo];
	if (algo < HASH_ALGO__LAST)
		return hash_algo_name[algo];

	log_err("digest %d not found\n", algo);
	return "unknown";
}

int get_filesize(const char *filename)
{
	struct stat stats;
	/*  Need to know the file length */
	stat(filename, &stats);
	return (int)stats.st_size;
}

static inline off_t get_fdsize(int fd)
{
	struct stat stats;
	/*  Need to know the file length */
	fstat(fd, &stats);
	return stats.st_size;
}

static int add_file_hash(const char *file, EVP_MD_CTX *ctx)
{
	uint8_t *data;
	int err = -1, bs = DATA_SIZE;
	off_t size, len;
	FILE *fp;

	fp = fopen(file, "r");
	if (!fp) {
		log_err("Failed to open: %s\n", file);
		return -1;
	}

	data = malloc(bs);
	if (!data) {
		log_err("malloc failed\n");
		goto out;
	}

	for (size = get_fdsize(fileno(fp)); size; size -= len) {
		len = MIN(size, bs);
		if (!fread(data, len, 1, fp)) {
			if (ferror(fp)) {
				log_err("fread() failed\n\n");
				goto out;
			}
			break;
		}
		if (!EVP_DigestUpdate(ctx, data, len)) {
			log_err("EVP_DigestUpdate() failed\n");
			err = 1;
			goto out;
		}
	}
	err = 0;
out:
	fclose(fp);
	free(data);

	return err;
}

static int add_dir_hash(const char *file, EVP_MD_CTX *ctx)
{
	int err;
	struct dirent *de;
	DIR *dir;
	unsigned long long ino, off;
	unsigned int type;
	int result = 0;

	dir = opendir(file);
	if (!dir) {
		log_err("Failed to open: %s\n", file);
		return -1;
	}

	while ((de = readdir(dir))) {
		ino = de->d_ino;
		off = de->d_off;
		type = de->d_type;
		log_debug("entry: %s, ino: %llu, type: %u, off: %llu, reclen: %hu\n",
			  de->d_name, ino, type, off, de->d_reclen);
		err = EVP_DigestUpdate(ctx, de->d_name, strlen(de->d_name));
		/*err |= EVP_DigestUpdate(ctx, &off, sizeof(off));*/
		err |= EVP_DigestUpdate(ctx, &ino, sizeof(ino));
		err |= EVP_DigestUpdate(ctx, &type, sizeof(type));
		if (!err) {
			log_err("EVP_DigestUpdate() failed\n");
			result = 1;
			break;
		}
	}

	closedir(dir);

	return result;
}

static int add_link_hash(const char *path, EVP_MD_CTX *ctx)
{
	int err;
	char buf[1024];

	err = readlink(path, buf, sizeof(buf));
	if (err <= 0)
		return -1;

	log_info("link: %s -> %.*s\n", path, err, buf);
	return !EVP_DigestUpdate(ctx, buf, err);
}

static int add_dev_hash(struct stat *st, EVP_MD_CTX *ctx)
{
	uint32_t dev = st->st_rdev;
	unsigned major = (dev & 0xfff00) >> 8;
	unsigned minor = (dev & 0xff) | ((dev >> 12) & 0xfff00);

	log_info("device: %u:%u\n", major, minor);
	return !EVP_DigestUpdate(ctx, &dev, sizeof(dev));
}

int ima_calc_hash(const char *file, uint8_t *hash)
{
	const EVP_MD *md;
	struct stat st;
	EVP_MD_CTX *pctx;
	unsigned int mdlen;
	int err;
#if OPENSSL_VERSION_NUMBER < 0x10100000
	EVP_MD_CTX ctx;
	pctx = &ctx;
#else
	pctx = EVP_MD_CTX_new();
#endif

	/*  Need to know the file length */
	err = lstat(file, &st);
	if (err < 0) {
		log_err("Failed to stat: %s\n", file);
		goto err;
	}

	md = EVP_get_digestbyname(params.hash_algo);
	if (!md) {
		log_err("EVP_get_digestbyname(%s) failed\n", params.hash_algo);
		err = 1;
		goto err;
	}

	err = EVP_DigestInit(pctx, md);
	if (!err) {
		log_err("EVP_DigestInit() failed\n");
		err = 1;
		goto err;
	}

	switch (st.st_mode & S_IFMT) {
	case S_IFREG:
		err = add_file_hash(file, pctx);
		break;
	case S_IFDIR:
		err = add_dir_hash(file, pctx);
		break;
	case S_IFLNK:
		err = add_link_hash(file, pctx);
		break;
	case S_IFIFO: case S_IFSOCK:
	case S_IFCHR: case S_IFBLK:
		err = add_dev_hash(&st, pctx);
		break;
	default:
		log_errno("Unsupported file type");
		err = -1;
		goto err;
	}

	if (err)
		goto err;

	err = EVP_DigestFinal(pctx, hash, &mdlen);
	if (!err) {
		log_err("EVP_DigestFinal() failed\n");
		err = 1;
		goto err;
	}
	err = mdlen;
err:
#if OPENSSL_VERSION_NUMBER >= 0x10100000
	EVP_MD_CTX_free(pctx);
#endif
	return err;
}

EVP_PKEY *read_pub_pkey(const char *keyfile, int x509)
{
	FILE *fp;
	X509 *crt = NULL;
	EVP_PKEY *pkey = NULL;

	fp = fopen(keyfile, "r");
	if (!fp) {
		log_err("Failed to open keyfile: %s\n", keyfile);
		return NULL;
	}

	if (x509) {
		crt = d2i_X509_fp(fp, NULL);
		if (!crt) {
			log_err("d2i_X509_fp() failed\n");
			goto out;
		}
		pkey = X509_extract_key(crt);
		X509_free(crt);
		if (!pkey) {
			log_err("X509_extract_key() failed\n");
			goto out;
		}
	} else {
		pkey = PEM_read_PUBKEY(fp, NULL, NULL, NULL);
		if (!pkey)
			log_err("PEM_read_PUBKEY() failed\n");
	}

out:
	fclose(fp);
	return pkey;
}

RSA *read_pub_key(const char *keyfile, int x509)
{
	EVP_PKEY *pkey;
	RSA *key;

	pkey = read_pub_pkey(keyfile, x509);
	if (!pkey)
		return NULL;
	key = EVP_PKEY_get1_RSA(pkey);
	EVP_PKEY_free(pkey);
	if (!key) {
		log_err("read_pub_key: unsupported key type\n");
		return NULL;
	}
	return key;
}

int verify_hash_v1(const char *file, const unsigned char *hash, int size,
		   unsigned char *sig, int siglen, const char *keyfile)
{
	int err, len;
	SHA_CTX ctx;
	unsigned char out[1024];
	RSA *key;
	unsigned char sighash[20];
	struct signature_hdr *hdr = (struct signature_hdr *)sig;

	log_info("hash-v1: ");
	log_dump(hash, size);

	key = read_pub_key(keyfile, 0);
	if (!key)
		return 1;

	SHA1_Init(&ctx);
	SHA1_Update(&ctx, hash, size);
	SHA1_Update(&ctx, hdr, sizeof(*hdr));
	SHA1_Final(sighash, &ctx);
	log_info("sighash: ");
	log_dump(sighash, sizeof(sighash));

	err = RSA_public_decrypt(siglen - sizeof(*hdr) - 2, sig + sizeof(*hdr) + 2, out, key, RSA_PKCS1_PADDING);
	RSA_free(key);
	if (err < 0) {
		log_err("%s: RSA_public_decrypt() failed: %d\n", file, err);
		return 1;
	}

	len = err;

	if (len != sizeof(sighash) || memcmp(out, sighash, len) != 0) {
		log_err("%s: verification failed: %d\n", file, err);
		return -1;
	}

	return 0;
}

struct public_key_entry {
	struct public_key_entry *next;
	uint32_t keyid;
	char name[9];
	EVP_PKEY *key;
};
static struct public_key_entry *public_keys = NULL;

static EVP_PKEY *find_keyid(uint32_t keyid)
{
	struct public_key_entry *entry;

	for (entry = public_keys; entry != NULL; entry = entry->next) {
		if (entry->keyid == keyid)
			return entry->key;
	}
	return NULL;
}

void init_public_keys(const char *keyfiles)
{
	struct public_key_entry *entry;
	char *tmp_keyfiles, *keyfiles_free;
	char *keyfile;
	int i = 1;

	tmp_keyfiles = strdup(keyfiles);
	keyfiles_free = tmp_keyfiles;

	while ((keyfile = strsep(&tmp_keyfiles, ", \t")) != NULL) {
		if (!keyfile)
			break;
		if ((*keyfile == '\0') || (*keyfile == ' ') ||
		    (*keyfile == '\t'))
			continue;

		entry = malloc(sizeof(struct public_key_entry));
		if (!entry) {
			perror("malloc");
			break;
		}

		entry->key = read_pub_pkey(keyfile, 1);
		if (!entry->key) {
			free(entry);
			continue;
		}

		calc_keyid_v2(&entry->keyid, entry->name, entry->key);
		sprintf(entry->name, "%x", __be32_to_cpup(&entry->keyid));
		log_info("key %d: %s %s\n", i++, entry->name, keyfile);
		entry->next = public_keys;
		public_keys = entry;
	}
	free(keyfiles_free);
}

/*
 * Return: 0 verification good, 1 verification bad, -1 error.
 */
int verify_hash_v2(const char *file, const unsigned char *hash, int size,
		   unsigned char *sig, int siglen, const char *keyfile)
{
	int ret = -1;
	EVP_PKEY *pkey, *pkey_free = NULL;
	struct signature_v2_hdr *hdr = (struct signature_v2_hdr *)sig;
	EVP_PKEY_CTX *ctx;
	const EVP_MD *md;
	const char *st;

	if (params.verbose > LOG_INFO) {
		log_info("hash(%s): ", params.hash_algo);
		log_dump(hash, size);
	}

	if (public_keys) {
		uint32_t keyid = hdr->keyid;

		pkey = find_keyid(keyid);
		if (!pkey) {
			log_err("%s: unknown keyid: %x\n", file,
				__be32_to_cpup(&keyid));
			return -1;
		}
	} else {
		pkey = read_pub_pkey(keyfile, 1);
		if (!pkey)
			return -1;
		pkey_free = pkey;
	}

	st = "EVP_PKEY_CTX_new";
	if (!(ctx = EVP_PKEY_CTX_new(pkey, NULL)))
		goto err;
	st = "EVP_PKEY_verify_init";
	if (!EVP_PKEY_verify_init(ctx))
		goto err;
	st = "EVP_get_digestbyname";
	if (!(md = EVP_get_digestbyname(params.hash_algo)))
		goto err;
	st = "EVP_PKEY_CTX_set_signature_md";
	if (!EVP_PKEY_CTX_set_signature_md(ctx, md))
		goto err;
	st = "EVP_PKEY_verify";
	ret = EVP_PKEY_verify(ctx, sig + sizeof(*hdr),
			      siglen - sizeof(*hdr), hash, size);
	if (ret == 1)
		ret = 0;
	else if (ret == 0) {
		log_err("%s: verification failed: %d (%s)\n",
			file, ret, ERR_reason_error_string(ERR_get_error()));
		ret = 1;
	}
err:
	if (ret < 0 || ret > 1) {
		log_err("%s: verification failed: %d (%s) in %s\n",
			file, ret, ERR_reason_error_string(ERR_peek_error()),
			st);
		ret = -1;
	}
	EVP_PKEY_CTX_free(ctx);
	EVP_PKEY_free(pkey_free);
	return ret;
}

int get_hash_algo(const char *algo)
{
	int i;

	/* first iterate over builtin algorithms */
	for (i = 0; i < PKEY_HASH__LAST; i++)
		if (pkey_hash_algo[i] &&
		    !strcmp(algo, pkey_hash_algo[i]))
			return i;

	for (i = 0; i < PKEY_HASH__LAST; i++)
		if (pkey_hash_algo_kern[i] &&
		    !strcmp(algo, pkey_hash_algo_kern[i]))
			return i;

	/* iterate over algorithms provided by kernel-headers */
	for (i = 0; i < HASH_ALGO__LAST; i++)
		if (hash_algo_name[i] &&
		    !strcmp(algo, hash_algo_name[i]))
			return i;

	log_info("digest %s not found, fall back to sha1\n", algo);
	return PKEY_HASH_SHA1;
}

static int get_hash_algo_from_sig(unsigned char *sig)
{
	uint8_t hashalgo;

	if (sig[0] == 1) {
		hashalgo = ((struct signature_hdr *)sig)->hash;

		if (hashalgo >= DIGEST_ALGO_MAX)
			return -1;

		switch (hashalgo) {
		case DIGEST_ALGO_SHA1:
			return PKEY_HASH_SHA1;
		case DIGEST_ALGO_SHA256:
			return PKEY_HASH_SHA256;
		default:
			return -1;
		}
	} else if (sig[0] == 2) {
		hashalgo = ((struct signature_v2_hdr *)sig)->hash_algo;
		if (hashalgo >= PKEY_HASH__LAST)
			return -1;
		return hashalgo;
	} else
		return -1;
}

int verify_hash(const char *file, const unsigned char *hash, int size, unsigned char *sig,
		int siglen)
{
	const char *key;
	int x509;
	verify_hash_fn_t verify_hash;

	/* Get signature type from sig header */
	if (sig[0] == DIGSIG_VERSION_1) {
		verify_hash = verify_hash_v1;
		/* Read pubkey from RSA key */
		x509 = 0;
	} else if (sig[0] == DIGSIG_VERSION_2) {
		verify_hash = verify_hash_v2;
		/* Read pubkey from x509 cert */
		x509 = 1;
	} else
		return -1;

	/* Determine what key to use for verification*/
	key = params.keyfile ? : x509 ?
			"/etc/keys/x509_evm.der" :
			"/etc/keys/pubkey_evm.pem";

	return verify_hash(file, hash, size, sig, siglen, key);
}

int ima_verify_signature(const char *file, unsigned char *sig, int siglen,
			 unsigned char *digest, int digestlen)
{
	unsigned char hash[MAX_DIGEST_SIZE];
	int hashlen, sig_hash_algo;

	if (sig[0] != 0x03) {
		log_err("xattr ima has no signature\n");
		return -1;
	}

	sig_hash_algo = get_hash_algo_from_sig(sig + 1);
	if (sig_hash_algo < 0) {
		log_err("Invalid signature\n");
		return -1;
	}
	/* Use hash algorithm as retrieved from signature */
	params.hash_algo = get_hash_algo_by_id(sig_hash_algo);

	/*
	 * Validate the signature based on the digest included in the
	 * measurement list, not by calculating the local file digest.
	 */
	if (digestlen > 0)
	    return verify_hash(file, digest, digestlen, sig + 1, siglen - 1);

	hashlen = ima_calc_hash(file, hash);
	if (hashlen <= 1)
		return hashlen;
	assert(hashlen <= sizeof(hash));

	return verify_hash(file, hash, hashlen, sig + 1, siglen - 1);
}

/*
 * Create binary key representation suitable for kernel
 */
int key2bin(RSA *key, unsigned char *pub)
{
	int len, b, offset = 0;
	struct pubkey_hdr *pkh = (struct pubkey_hdr *)pub;
	const BIGNUM *n, *e;

#if OPENSSL_VERSION_NUMBER < 0x10100000
	n = key->n;
	e = key->e;
#else
	RSA_get0_key(key, &n, &e, NULL);
#endif

	/* add key header */
	pkh->version = 1;
	pkh->timestamp = 0;	/* PEM has no timestamp?? */
	pkh->algo = PUBKEY_ALGO_RSA;
	pkh->nmpi = 2;

	offset += sizeof(*pkh);

	len = BN_num_bytes(n);
	b = BN_num_bits(n);
	pub[offset++] = b >> 8;
	pub[offset++] = b & 0xff;
	BN_bn2bin(n, &pub[offset]);
	offset += len;

	len = BN_num_bytes(e);
	b = BN_num_bits(e);
	pub[offset++] = b >> 8;
	pub[offset++] = b & 0xff;
	BN_bn2bin(e, &pub[offset]);
	offset += len;

	return offset;
}

void calc_keyid_v1(uint8_t *keyid, char *str, const unsigned char *pkey, int len)
{
	uint8_t sha1[SHA_DIGEST_LENGTH];
	uint64_t id;

	SHA1(pkey, len, sha1);

	/* sha1[12 - 19] is exactly keyid from gpg file */
	memcpy(keyid, sha1 + 12, 8);
	log_debug("keyid: ");
	log_debug_dump(keyid, 8);
	id = __be64_to_cpup((__be64 *) keyid);
	sprintf(str, "%llX", (unsigned long long)id);

	if (params.verbose > LOG_INFO)
		log_info("keyid-v1: %s\n", str);
}

/*
 * Calculate keyid of the public_key part of EVP_PKEY
 */
void calc_keyid_v2(uint32_t *keyid, char *str, EVP_PKEY *pkey)
{
	X509_PUBKEY *pk = NULL;
	const unsigned char *public_key = NULL;
	int len;

	/* This is more generic than i2d_PublicKey() */
	if (X509_PUBKEY_set(&pk, pkey) &&
	    X509_PUBKEY_get0_param(NULL, &public_key, &len, NULL, pk)) {
		uint8_t sha1[SHA_DIGEST_LENGTH];

		SHA1(public_key, len, sha1);
		/* sha1[12 - 19] is exactly keyid from gpg file */
		memcpy(keyid, sha1 + 16, 4);
	} else
		*keyid = 0;

	log_debug("keyid: ");
	log_debug_dump(keyid, 4);
	sprintf(str, "%x", __be32_to_cpup(keyid));

	if (params.verbose > LOG_INFO)
		log_info("keyid: %s\n", str);

	X509_PUBKEY_free(pk);
}

static EVP_PKEY *read_priv_pkey(const char *keyfile, const char *keypass)
{
	FILE *fp;
	EVP_PKEY *pkey;

	fp = fopen(keyfile, "r");
	if (!fp) {
		log_err("Failed to open keyfile: %s\n", keyfile);
		return NULL;
	}
	ERR_load_crypto_strings();
	pkey = PEM_read_PrivateKey(fp, NULL, NULL, (void *)keypass);
	if (!pkey) {
		char str[256];

		ERR_error_string(ERR_get_error(), str);
		log_err("PEM_read_PrivateKey() failed: %s\n", str);
	}

	fclose(fp);
	return pkey;
}

static RSA *read_priv_key(const char *keyfile, const char *keypass)
{
	EVP_PKEY *pkey;
	RSA *key;

	pkey = read_priv_pkey(keyfile, keypass);
	if (!pkey)
		return NULL;
	key = EVP_PKEY_get1_RSA(pkey);
	EVP_PKEY_free(pkey);
	if (!key) {
		log_err("read_priv_key: unsupported key type\n");
		return NULL;
	}
	return key;
}

static int get_hash_algo_v1(const char *algo)
{

	if (!strcmp(algo, "sha1"))
		return DIGEST_ALGO_SHA1;
	else if (!strcmp(algo, "sha256"))
		return DIGEST_ALGO_SHA256;

	return -1;
}

int sign_hash_v1(const char *hashalgo, const unsigned char *hash, int size, const char *keyfile, unsigned char *sig)
{
	int len = -1, hashalgo_idx;
	SHA_CTX ctx;
	unsigned char pub[1024];
	RSA *key;
	char name[20];
	unsigned char sighash[20];
	struct signature_hdr *hdr;
	uint16_t *blen;

	if (!hash) {
		log_err("sign_hash_v1: hash is null\n");
		return -1;
	}

	if (size < 0) {
		log_err("sign_hash_v1: size is negative: %d\n", size);
		return -1;
	}

	if (!hashalgo) {
		log_err("sign_hash_v1: hashalgo is null\n");
		return -1;
	}

	if (!sig) {
		log_err("sign_hash_v1: sig is null\n");
		return -1;
	}

	log_info("hash: ");
	log_dump(hash, size);

	key = read_priv_key(keyfile, params.keypass);
	if (!key)
		return -1;

	hdr = (struct signature_hdr *)sig;

	/* now create a new hash */
	hdr->version = (uint8_t) DIGSIG_VERSION_1;
	hdr->timestamp = time(NULL);
	hdr->algo = PUBKEY_ALGO_RSA;
	hashalgo_idx = get_hash_algo_v1(hashalgo);
	if (hashalgo_idx < 0) {
		log_err("Signature version 1 does not support hash algo %s\n",
			hashalgo);
		goto out;
	}
	hdr->hash = (uint8_t) hashalgo_idx;

	len = key2bin(key, pub);
	calc_keyid_v1(hdr->keyid, name, pub, len);

	hdr->nmpi = 1;

	SHA1_Init(&ctx);
	SHA1_Update(&ctx, hash, size);
	SHA1_Update(&ctx, hdr, sizeof(*hdr));
	SHA1_Final(sighash, &ctx);
	log_info("sighash: ");
	log_dump(sighash, sizeof(sighash));

	len = RSA_private_encrypt(sizeof(sighash), sighash, sig + sizeof(*hdr) + 2, key, RSA_PKCS1_PADDING);
	if (len < 0) {
		log_err("RSA_private_encrypt() failed: %d\n", len);
		goto out;
	}

	/* we add bit length of the signature to make it gnupg compatible */
	blen = (uint16_t *) (sig + sizeof(*hdr));
	*blen = __cpu_to_be16(len << 3);
	len += sizeof(*hdr) + 2;
	log_info("evm/ima signature-v1: %d bytes\n", len);
out:
	RSA_free(key);
	return len;
}

/*
 * @sig is assumed to be of (MAX_SIGNATURE_SIZE - 1) size
 * Return: -1 signing error, >0 length of signature
 */
int sign_hash_v2(const char *algo, const unsigned char *hash, int size, const char *keyfile, unsigned char *sig)
{
	struct signature_v2_hdr *hdr;
	int len = -1;
	EVP_PKEY *pkey;
	char name[20];
	EVP_PKEY_CTX *ctx = NULL;
	const EVP_MD *md;
	size_t sigsize;
	const char *st;
	uint32_t keyid;

	if (!hash) {
		log_err("sign_hash_v2: hash is null\n");
		return -1;
	}

	if (size < 0) {
		log_err("sign_hash_v2: size is negative: %d\n", size);
		return -1;
	}

	if (!sig) {
		log_err("sign_hash_v2: sig is null\n");
		return -1;
	}

	if (!algo) {
		log_err("sign_hash_v2: algo is null\n");
		return -1;
	}

	log_info("hash(%s): ", params.hash_algo);
	log_dump(hash, size);

	pkey = read_priv_pkey(keyfile, params.keypass);
	if (!pkey)
		return -1;

	hdr = (struct signature_v2_hdr *)sig;
	hdr->version = (uint8_t) DIGSIG_VERSION_2;

	hdr->hash_algo = get_hash_algo(algo);

	calc_keyid_v2(&keyid, name, pkey);
	hdr->keyid = keyid;

	st = "EVP_PKEY_CTX_new";
	if (!(ctx = EVP_PKEY_CTX_new(pkey, NULL)))
		goto err;
	st = "EVP_PKEY_sign_init";
	if (!EVP_PKEY_sign_init(ctx))
		goto err;
	st = "EVP_get_digestbyname";
	if (!(md = EVP_get_digestbyname(params.hash_algo)))
		goto err;
	st = "EVP_PKEY_CTX_set_signature_md";
	if (!EVP_PKEY_CTX_set_signature_md(ctx, md))
		goto err;
	st = "EVP_PKEY_sign";
	sigsize = MAX_SIGNATURE_SIZE - sizeof(struct signature_v2_hdr) - 1;
	if (!EVP_PKEY_sign(ctx, hdr->sig, &sigsize, hash, size))
		goto err;
	len = (int)sigsize;

	/* we add bit length of the signature to make it gnupg compatible */
	hdr->sig_size = __cpu_to_be16(len);
	len += sizeof(*hdr);
	log_info("evm/ima signature: %d bytes\n", len);

err:
	if (len == -1)
		log_err("sign_hash_v2: signing failed: (%s) in %s\n",
			ERR_reason_error_string(ERR_peek_error()), st);
	EVP_PKEY_CTX_free(ctx);
	EVP_PKEY_free(pkey);
	return len;
}


int sign_hash(const char *hashalgo, const unsigned char *hash, int size, const char *keyfile, const char *keypass, unsigned char *sig)
{
	if (keypass)
		params.keypass = keypass;

	return params.x509 ? sign_hash_v2(hashalgo, hash, size, keyfile, sig) :
			     sign_hash_v1(hashalgo, hash, size, keyfile, sig);
}

static void libinit()
{
	OPENSSL_init_crypto(OPENSSL_INIT_ADD_ALL_CIPHERS |
			    OPENSSL_INIT_ADD_ALL_DIGESTS, NULL);
	ERR_load_crypto_strings();
}
