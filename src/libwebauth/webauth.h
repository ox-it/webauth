/** @file
 * Interface to the libwebauth utility library.
 *
 * The libwebauth utility library contains the basic token handling functions
 * used by all other parts of the webauth code.  It contains functions to
 * encode and decode lists of attributes, generate tokens from them, encode
 * and decode tokens in base64 or hex encoding, and some additional utility
 * functions to generate random numbers or new AES keys.
 *
 * $Id$
 */

#ifndef _WEBAUTH_H
#define _WEBAUTH_H

#include <time.h>

#ifdef  __cplusplus
extern "C" {
#endif

/******************** error codes ********************/

/** libwebauth error codes.
 *
 * Many libwebauth functions return an error status, or 0 on success.  For
 * those functions, the error codes are chosen from the following enum.
 */
typedef enum {
    WA_ERR_NO_ROOM = -2000,  /**< Supplied buffer too small. */
    WA_ERR_CORRUPT,          /**< Data is incorrectly formatted. */
    WA_ERR_NO_MEM,           /**< No memory. */
    WA_ERR_BAD_HMAC,         /**< HMAC check failed. */
    WA_ERR_RAND_FAILURE,     /**< Unable to get random data. */
    WA_ERR_BAD_KEY,          /**< Unable to use key. */
    WA_ERR_KEYRING_WRITE,    /**< Unable to write to key ring. */
    WA_ERR_KEYRING_READ,     /**< Unable to read key ring file. */
    WA_ERR_KEYRING_VERSION,  /**< Bad keyring version. */
    WA_ERR_NOT_FOUND,        /**< Item not found while searching. */
    /* must be last */
    WA_ERR_NONE = 0          /**< No error occured. */
    /* must be last */
}  WEBAUTH_ERR;    

/******************** constants for token attributes **********/

#define WA_TK_APP_NAME "an"
#define WA_TK_CRED_DATA "crd"
#define WA_TK_CRED_TYPE "crt"
#define WA_TK_CREATION_TIME "ct"
#define WA_TK_ERROR_CODE "ec"
#define WA_TK_ERROR_MESSAGE "em"
#define WA_TK_EXPIRATION_TIME "et"
#define WA_TK_INACTIVITY_TIMEOUT "it"
#define WA_TK_SESSION_KEY "k"
#define WA_TK_LASTUSED_TIME "lt"
#define WA_TK_PROXY_TYPE "prt"
#define WA_TK_PROXY_DATA "prd"
#define WA_TK_PROXY_OWNER "pro"
#define WA_TK_POST_URL "pu"
#define WA_TK_REQUEST_REASON "rr"
#define WA_TK_REQUESTED_TOKEN_TYPE "rt"
#define WA_TK_REQUESTED_TOKEN_HASH "rth"
#define WA_TK_RETURN_URL "ru"
#define WA_TK_SUBJECT "s"
#define WA_TK_SUBJECT_AUTHENTICATOR "sa"
#define WA_TK_SERVICE_AUTHENTICATOR_NAME "san"
#define WA_TK_TOKEN_TYPE "t"
#define WA_TK_TOKEN_VERSION "ver"

/******************** other constants *****************/

/* supported key types */
#define WA_AES_KEY 1

/* supported AES key sizes */
#define WA_AES_128 16
#define WA_AES_192 24
#define WA_AES_256 32

/******************** types ********************/

/** A generic name/value attribute.
 *
 * Holds a generic name/value attribute for constructing and parsing tokens.
 * Names <b>must not</b> contain "=", and values <b>may</b> contain binary
 * data, since the length <b>must</b> be specified.
 */
typedef struct {
    char *name;                 /**< Name of attribute. */
    void *value;                /**< Value of attribute (binary data). */
    int length;                 /**< Length of attribute value in bytes. */
} WEBAUTH_ATTR;

/** An attribute list.
 *
 * Holds a list of attributes. You must always use use webauth_attr_list_new
 * to construct a new attr list, so webauth_attr_list_{add,free} work
 * correctly.
 */
typedef struct {
    int num_attrs;
    int capacity;
    WEBAUTH_ATTR *attrs;
} WEBAUTH_ATTR_LIST;

/* a crypto key */
typedef struct {
    int type;
    unsigned char *data;
    int length;
} WEBAUTH_KEY;

/* a key ring to hold private keys */
typedef struct {
    time_t creation_time;
    time_t valid_from;
    time_t valid_till;
    WEBAUTH_KEY *key;
} WEBAUTH_KEYRING_ENTRY;

typedef struct {
    int num_entries;
    int capacity;
    WEBAUTH_KEYRING_ENTRY *entries;
} WEBAUTH_KEYRING;


/******************** base64 ********************/

/** Amount of space required to base64-encode data.
 *
 * Returns the amount of space required to base64-encode data.  Returned
 * length does <b>NOT</b> include room for nul-termination.
 *
 * @param length Length of data to be encoded.
 * @return Space base64-encoded data will require.
 */
int webauth_base64_encoded_length(int length);

/** Amount of space required to base64-decode data.
 *
 * Returns the amount of space required to base64-decode data of the given
 * length.  Does not actually attempt to ensure that the input contains a
 * valid base64-encoded string, other than checking the last two characters
 * for padding ("=").  Returned length does <b>NOT</b> include room for
 * nul-termination.
 *
 * @param input Base64-encoded data.
 * @param length Length of base64-encoded data.
 *
 * @return Returns the required space in bytes provided that length is
 *   greater than 0 and a multiple of 4.  Otherwise, returns #WA_ERR_CORRUPT
 *   since the input data cannot be valid base64-encoded data.
 */
int webauth_base64_decoded_length(const unsigned char *input, int length);

/** Base64-encode the given data.
 *
 * Does <b>NOT</b> nul-terminate.  Output cannot point to the same memory
 * space as input.
 *
 * @param input Data to encode.
 * @param input_len Length of data to encode.
 * @param output Buffer into which to write base64-encoded data.
 * @param max_output_len Maximum number of bytes to write to @a output.
 *
 * @return Returns the number of bytes written to @a output, or
 *   #WA_ERR_NO_ROOM if encoding the provided data would require more space
 *   than @a max_output_len.
 */
int webauth_base64_encode(const unsigned char *input,
                          int input_len, 
                          unsigned char *output,
                          int max_output_len);

/** Base64-decode the given data.
 *
 * Does <b>NOT</b> nul-terminate.  Output may point to input.
 *
 * @param input Data to decode.
 * @param input_len Length of data to decode.
 * @param output Buffer into which to write base64-decoded data.
 * @param max_output_len Maximum number of bytes to write to @a output.
 *
 * @return Returns the number of bytes written to @a output, #WA_ERR_NO_ROOM
 *   if decoding the provided data would require more space than @a
 *   max_output_len, or #WA_ERR_CORRUPT if @a input is not valid
 *   base64-encoded data.
 */
int webauth_base64_decode(unsigned char *input,
                          int input_len,
                          unsigned char *output,
                          int max_output_len);

/******************** hex routines ********************/

/*
 * returns the amount of space required to hex encode data
 * of the given length. Returned length does *NOT* include room for a
 * null-termination.
 */
int webauth_hex_encoded_length(int length);


/*
 * returns the amount of space required to decode the hex encoded data
 * of the given length. Returned length does *NOT* include room for a
 * null-termination. 
 *
 * errors:
 *   WA_ERR_CORRUPT (if length is not greater then 0 and a multiple of 2)
 */
int webauth_hex_decoded_length(int length);

/*
 * hex encodes the given data, does *NOT* null-terminate.
 * output can point to input, as long as max_output_len is
 * long enough.
 *
 * returns output length or an error.
 *
 * errors:
 *   WA_ERR_NO_ROOM
 *   
 */
int webauth_hex_encode(unsigned char *input, 
                       int input_len,
                       unsigned char *output,
                       int max_output_len);


/*
 * hex decodes the given data, does *NOT* null-terminate.
 * output can point to input.
 *
 * returns output length or an error.
 *
 * errors:
 *   WA_ERR_NO_ROOM
 *   WA_ERR_CORRUPT
 */
int webauth_hex_decode(unsigned char *input,
                       int input_len,
                       unsigned char *output, 
                       int max_output_len);

/******************** attrs ********************/

/** Creates a new attr list
 * 
 * Returns the new attr list, or NULL if out of memory
 *
 */
WEBAUTH_ATTR_LIST *webauth_attr_list_new();

/** adds an attr to the attr list.
 *
 * adds a new attribute to the given attr list, growing the list
 * if need be. Both the name and value are copied, and value
 * always has a null added to the end of it.
 *
 * if vlen is 0, then strlen is used on (char*)value.
 *
 * return WA_ERR_NONE or WA_ERR_NO_MEM
 *
 */
int webauth_attr_list_add(WEBAUTH_ATTR_LIST *list,
                          char *name, void *value, int vlen);

/** searches for the name attribute in the list and returns the
 * index or WA_ERR_NOT_FOUND.
 *
 */
int webauth_attr_list_find(WEBAUTH_ATTR_LIST *list, char *name);

/** free's an attr list
 *
 * Frees the memory associated with an attribute list, including
 * all the attrs in the list.
 */
void webauth_attr_list_free(WEBAUTH_ATTR_LIST *list);

/*
 * given an array of attributes, returns the amount
 * of space required to encode them.
 */

int webauth_attrs_encoded_length(const WEBAUTH_ATTR_LIST *list);

/*
 * given an array of attributes, encode them into the buffer.
 * max_buffer_len must be set to the maxium size of the output buffer.
 *
 * output is *NOT* null-terminated
 *
 * returns length of encoded data or an error
 *
 * errors:
 *   WA_ERR_NO_ROOM
 */

int webauth_attrs_encode(const WEBAUTH_ATTR_LIST *list,
                         unsigned char *output,
                         int max_output_len);

/*
 * decodes the given buffer into an array of attributes.
 * The buffer is modifed.
 *
 * returns the number of attributes decoded or an error
 *
 * errors:
 *   WA_ERR_CORRUPT
 *   WA_ERR_NO_MEM
 */

int webauth_attrs_decode(unsigned char *buffer, 
                         int buffer_len,
                         WEBAUTH_ATTR_LIST **list);


/******************** random data ********************/

/*
 * returns pseudo random bytes, suitable for use a nonce
 * or random data, but not necessarily suitable for use
 * as an encryption key. Use webauth_random_key for that.
 * The number of bytes specified by output_len is placed in
 * output, which must contain enough room to contain the
 * requested number of bytes.
 *
 * returns WA_ERR_NONE on success, or WA_ERR_RAND_FAILURE on error.
 */
int webauth_random_bytes(unsigned char *output, int num_bytes);

/*
 * used to create random bytes suitable for use as a key.
 * The number of bytes specified in key_len is placed in key, which
 * must contain enough room to hold key_len byte of data.
 *
 * returns WA_ERR_NONE on success, or WA_ERR_RAND_FAILURE on error.
 */

int webauth_random_key(unsigned char *key, int key_len);

/******************** keys ********************/

/*
 * construct new key. 
 *
 * key_type is the key type, and currently the only supported type
 * is WA_AES_KEY.
 *
 * key_material points to the key material, and will get coppied
 * into the new key.
 *
 * key_len is the length of the key material and should
 * be WA_AES_128, WA_AES_192, or WA_AES_256.
 * 
 * returns newly allocated key, or NULL on error
 *
 */

WEBAUTH_KEY *webauth_key_create(int key_type,
                                const unsigned char *key_material,
                                int key_len);

/*
 * copies a key
 */

WEBAUTH_KEY *webauth_key_copy(const WEBAUTH_KEY *key);

/*
 * zeros out key memory and then frees it
 */

void webauth_key_free(WEBAUTH_KEY *key);

/*
 * create a new key ring
 */
WEBAUTH_KEYRING * webauth_keyring_new(int initial_capacity);

/*
 * free a key ring, and any keys in it
 */
void webauth_keyring_free(WEBAUTH_KEYRING *ring);

/*
 * add a new entry to a key ring. After the call, "key" will
 * be owned by the key ring, and will be freed when the 
 * the key ring is freed.
 *
 * If creation_time or valid_from time is 0, then the current time is used.
 */

int webauth_keyring_add(WEBAUTH_KEYRING *ring,
                         time_t creation_time,
                         time_t valid_from,
                         time_t valid_till,
                         WEBAUTH_KEY *key);

/*
 * given a key ring, return the best key on the ring for
 * either encryption or decryption.
 *
 * The best key for encryption is the key with
 * a valid valid_from time, and the latest valid valid_till time.
 *
 * The "best" key for decryption is the key where hint
 * is between valid_from and valid_till.
 *
 */
WEBAUTH_KEY *webauth_keyring_best_key(const WEBAUTH_KEYRING *ring,
                                      int encryption,
                                      time_t hint);

/*
 * given a key ring, return the best key on the ring for
 * encryption. The best key for encryption is the key with
 * a valid valid_from time, and the latest
 * valid valid_till time.
 */
WEBAUTH_KEY *
 webauth_keyring_best_decryption_key(const WEBAUTH_KEYRING *ring, time_t hint);

/*
 * write a key ring to a file. 
 *
 * returns WA_ERR_NONE on success, or an error.
 */
int webauth_keyring_write_file(WEBAUTH_KEYRING *ring, char *path);

/*
 * reads a key ring from a file.
 *
 * returns WA_ERR_NONE on success, or an error.
 */

int
webauth_keyring_read_file(char *path, WEBAUTH_KEYRING **ring);

/******************** tokens ********************/
   
/*
 * returns length required to encrypt+base64 encode token,
 * not including null-termination.
 */
int webauth_token_encoded_length(const WEBAUTH_ATTR_LIST *list);

/*
 * encrypts and base64 encodes attrs into a token, using the
 * key from the key ring that has a valid valid_from time and
 * the latest valid_to time.
 *
 * if hint is 0 then the current time will be used.
 *
 * returns length of base64-encoded token (not null-terminated) or an error
 *
 * errors:
 *  WA_ERR_NO_ROOM
 *  WA_ERR_NO_MEM
 *  WA_ERR_BAD_KEY
 *  
 */
int webauth_token_create(const WEBAUTH_ATTR_LIST *list,
                         time_t hint,
                         unsigned char *output,
                         int max_output_len,
                         const WEBAUTH_KEYRING *ring);

/*
 * base64 decodes and decrypts attrs into a token
 * input buffer is modified.
 *
 * list will point to the dynamically-allocated list
 * of attrs and must be freed when no longer needed.
 *
 * returns number of attrs in the resulting token or an error
 *
 * errors:
 *  WA_ERR_NO_MEM
 *  WA_ERR_CORRUPT
 *  WA_ERR_BAD_HMAC
 *  WA_ERR_BAD_KEY
 */

int webauth_token_parse(unsigned char *input,
                        int input_len,
                        WEBAUTH_ATTR_LIST **list,
                        const WEBAUTH_KEYRING *ring);

#ifdef  __cplusplus
}
#endif

/*
**  Local variables:
**  mode: c
**  c-basic-offset: 4
**  indent-tabs-mode: nil
**  end:
*/

#endif
