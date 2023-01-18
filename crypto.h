#ifndef _CRYPTO_H
#define _CRYPTO_H

#include "tools-util.h"
#include "rust-src/bcachefs_rs/src/bcachefs_rs.h"

struct bch_sb;
struct bch_sb_field_crypt;
struct bch_key;
struct bch_encrypted_key;

char *read_passphrase_twice(const char *);

struct bch_key derive_passphrase(struct bch_sb_field_crypt *, const char *);
bool bch2_sb_is_encrypted(struct bch_sb *);
void bch2_passphrase_check(struct bch_sb *, const char *,
			   struct bch_key *, struct bch_encrypted_key *);
void bch2_add_key(struct bch_sb *, const char *, const char *, const char *);
void bch_sb_crypt_init(struct bch_sb *sb, struct bch_sb_field_crypt *,
		       const char *);

#endif /* _CRYPTO_H */
