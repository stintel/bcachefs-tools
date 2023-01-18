#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <keyutils.h>
#include <linux/random.h>
#include <sodium/crypto_pwhash_scryptsalsa208sha256.h>
#include <uuid/uuid.h>

#include "libbcachefs/checksum.h"
#include "crypto.h"

char *read_passphrase_twice(const char *prompt)
{
	char *pass = read_passphrase(prompt);

	if (!isatty(STDIN_FILENO))
		return pass;

	char *pass2 = read_passphrase("Enter same passphrase again: ");

	if (strcmp(pass, pass2)) {
		free_cstring(pass);
		free_cstring(pass2);
		die("Passphrases do not match");
	}

	free_cstring(pass);
	free_cstring(pass2);

	return pass;
}

struct bch_key derive_passphrase(struct bch_sb_field_crypt *crypt,
				 const char *passphrase)
{
	const unsigned char salt[] = "bcache";
	struct bch_key key;
	int ret;

	switch (BCH_CRYPT_KDF_TYPE(crypt)) {
	case BCH_KDF_SCRYPT:
		ret = crypto_pwhash_scryptsalsa208sha256_ll(
			(void *) passphrase, strlen(passphrase),
			salt, sizeof(salt),
			1ULL << BCH_KDF_SCRYPT_N(crypt),
			1ULL << BCH_KDF_SCRYPT_R(crypt),
			1ULL << BCH_KDF_SCRYPT_P(crypt),
			(void *) &key, sizeof(key));
		if (ret)
			die("scrypt error: %i", ret);
		break;
	default:
		die("unknown kdf type %llu", BCH_CRYPT_KDF_TYPE(crypt));
	}

	return key;
}

bool bch2_sb_is_encrypted(struct bch_sb *sb)
{
	struct bch_sb_field_crypt *crypt;

	return (crypt = bch2_sb_get_crypt(sb)) &&
		bch2_key_is_encrypted(&crypt->key);
}

void bch2_passphrase_check(struct bch_sb *sb, const char *passphrase,
			   struct bch_key *passphrase_key,
			   struct bch_encrypted_key *sb_key)
{
	struct bch_sb_field_crypt *crypt = bch2_sb_get_crypt(sb);
	if (!crypt)
		die("filesystem is not encrypted");

	*sb_key = crypt->key;

	if (!bch2_key_is_encrypted(sb_key))
		die("filesystem does not have encryption key");

	*passphrase_key = derive_passphrase(crypt, passphrase);

	/* Check if the user supplied the correct passphrase: */
	if (bch2_chacha_encrypt_key(passphrase_key, __bch2_sb_key_nonce(sb),
				    sb_key, sizeof(*sb_key)))
		die("error encrypting key");

	if (bch2_key_is_encrypted(sb_key))
		die("incorrect passphrase");
}

void bch2_add_key(struct bch_sb *sb,
		  const char *type,
		  const char *keyring_str,
		  const char *passphrase)
{
	struct bch_key passphrase_key;
	struct bch_encrypted_key sb_key;
	int keyring;

	if (!strcmp(keyring_str, "session"))
		keyring = KEY_SPEC_SESSION_KEYRING;
	else if (!strcmp(keyring_str, "user"))
		keyring = KEY_SPEC_USER_KEYRING;
	else if (!strcmp(keyring_str, "user_session"))
		keyring = KEY_SPEC_USER_SESSION_KEYRING;
	else
		die("unknown keyring %s", keyring_str);

	bch2_passphrase_check(sb, passphrase,
			      &passphrase_key,
			      &sb_key);

	char uuid[40];
	uuid_unparse_lower(sb->user_uuid.b, uuid);

	char *description = mprintf("bcachefs:%s", uuid);

	if (add_key(type,
		    description,
		    &passphrase_key, sizeof(passphrase_key),
		    keyring) < 0)
		die("add_key error: %m");

	memzero_explicit(description, strlen(description));
	free(description);
	memzero_explicit(&passphrase_key, sizeof(passphrase_key));
	memzero_explicit(&sb_key, sizeof(sb_key));
}

void bch_sb_crypt_init(struct bch_sb *sb,
		       struct bch_sb_field_crypt *crypt,
		       const char *passphrase)
{
	crypt->key.magic = BCH_KEY_MAGIC;
	get_random_bytes(&crypt->key.key, sizeof(crypt->key.key));

	if (passphrase) {

		SET_BCH_CRYPT_KDF_TYPE(crypt, BCH_KDF_SCRYPT);
		SET_BCH_KDF_SCRYPT_N(crypt, ilog2(16384));
		SET_BCH_KDF_SCRYPT_R(crypt, ilog2(8));
		SET_BCH_KDF_SCRYPT_P(crypt, ilog2(16));

		struct bch_key passphrase_key = derive_passphrase(crypt, passphrase);

		assert(!bch2_key_is_encrypted(&crypt->key));

		if (bch2_chacha_encrypt_key(&passphrase_key, __bch2_sb_key_nonce(sb),
					   &crypt->key, sizeof(crypt->key)))
			die("error encrypting key");

		assert(bch2_key_is_encrypted(&crypt->key));

		memzero_explicit(&passphrase_key, sizeof(passphrase_key));
	}
}
