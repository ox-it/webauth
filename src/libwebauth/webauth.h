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
/*extern "C" {*/
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
    WA_ERR_KEYRING_OPENWRITE,/**< Unable to open key ring for writing. */
    WA_ERR_KEYRING_WRITE,    /**< Unable to write to key ring. */
    WA_ERR_KEYRING_OPENREAD, /**< Unable to open key ring for reading. */
    WA_ERR_KEYRING_READ,     /**< Unable to read key ring file. */
    WA_ERR_KEYRING_VERSION,  /**< Bad keyring version. */
    WA_ERR_NOT_FOUND,        /**< Item not found while searching. */
    WA_ERR_KRB5,             /**< A Kerberos5 error occured. */
    WA_ERR_INVALID_CONTEXT,  /**< Invalid context passed to function. */
    WA_ERR_LOGIN_FAILED,     /**< Bad username/password. */
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

/* krb5 context */
typedef struct webauth_krb5_ctxt WEBAUTH_KRB5_CTXT;

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
 * @param decoded_length Length of decoded data
 *
 * @return #WA_ERR_NONE on success, or #WA_ERR_CORRUPT if
 *   length is not greater than 0 and a multiple of 4, 
 *   since the input data cannot be valid base64-encoded data.
 */
int webauth_base64_decoded_length(const unsigned char *input, 
                                  int length,
                                  int *decoded_length);

/** Base64-encode the given data.
 *
 * Does <b>NOT</b> nul-terminate.  Output cannot point to the same memory
 * space as input.
 *
 * @param input Data to encode.
 * @param input_len Length of data to encode.
 * @param output Buffer into which to write base64-encoded data.
 * @param output_len number of bytes written to output.
 * @param max_output_len Maximum number of bytes to write to @a output.
 *
 * @return Returns #WA_ERR_NONE on success, or
 *   #WA_ERR_NO_ROOM if encoding the provided data would require more space
 *   than @a max_output_len.
 */
int webauth_base64_encode(const unsigned char *input,
                          int input_len, 
                          unsigned char *output,
                          int *output_len,
                          int max_output_len);

/** Base64-decode the given data.
 *
 * Does <b>NOT</b> nul-terminate.  Output may point to input.
 *
 * @param input Data to decode.
 * @param input_len Length of data to decode.
 * @param output Buffer into which to write base64-decoded data.
 * @param output_length Number of bytes written to output.
 * @param max_output_len Maximum number of bytes to write to @a output.
 *
 * @return Returns #WA_ERR_NONE on success, #WA_ERR_NO_ROOM
 *   if decoding the provided data would require more space than @a
 *   max_output_len, or #WA_ERR_CORRUPT if @a input is not valid
 *   base64-encoded data.
 */
int webauth_base64_decode(unsigned char *input,
                          int input_len,
                          unsigned char *output,
                          int *output_len,
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
 * @return #WA_ERR_NONE on succes, or #WA_ERR_CORRUPT 
 * if length is not greater then 0 and a multiple of 2.
 */
int webauth_hex_decoded_length(int length, int *out_length);

/*
 * hex encodes the given data, does *NOT* null-terminate.
 * output can point to input, as long as max_output_len is
 * long enough.
 *
 * @return #WA_ERR_NONE or #WA_ERR_NO_ROOM
 *
 */
int webauth_hex_encode(unsigned char *input, 
                       int input_len,
                       unsigned char *output,
                       int *output_len,
                       int max_output_len);


/*
 * hex decodes the given data, does *NOT* null-terminate.
 * output can point to input.
 *
 * @return #WA_ERR_NONE , #WA_ERR_NO_ROOM, #WA_ERR_CORRUPT
 *
 */
int webauth_hex_decode(unsigned char *input,
                       int input_len,
                       unsigned char *output,
                       int *output_length,
                       int max_output_len);

/******************** attrs ********************/

/** Creates a new attr list
 * 
 * Returns the new attr list, or NULL if out of memory
 *
 */
WEBAUTH_ATTR_LIST *webauth_attr_list_new(int initial_capacity);

/** adds an attr to the attr list.
 *
 * adds a new attribute to the given attr list, growing the list
 * if need be. Both the name and value are copied, and value
 * always has a null added to the end of it.
 *
 * return WA_ERR_NONE or WA_ERR_NO_MEM
 *
 */
int webauth_attr_list_add(WEBAUTH_ATTR_LIST *list,
                          const char *name, const void *value, int vlen);

/** adds an attr string to the attr list.
 *
 * adds a new attribute to the given attr list, growing the list
 * if need be. Both the name and value are copied, and value
 * always has a null added to the end of it.
 *
 * if vlen is 0, then strlen(value) is used.
 *
 * return WA_ERR_NONE or WA_ERR_NO_MEM
 *
 */
int webauth_attr_list_add_str(WEBAUTH_ATTR_LIST *list,
                              const char *name, const char *value, int vlen);

/** searches for the name attribute in the list and returns the
 * index or WA_ERR_NOT_FOUND.
 *
 */
int webauth_attr_list_find(WEBAUTH_ATTR_LIST *list, const char *name);

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
 * returns WA_ERR_NONE on success, or an error.
 *
 * errors:
 *   WA_ERR_NO_ROOM
 */

int webauth_attrs_encode(const WEBAUTH_ATTR_LIST *list,
                         unsigned char *output,
                         int *output_len,
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

int webauth_keyring_read_file(char *path, WEBAUTH_KEYRING **ring);

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
 * returns WA_ERR_NONE or an error.
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
                         int *output_len,
                         int max_output_len,
                         const WEBAUTH_KEYRING *ring);


/*
 * encrypts and base64 encodes attrs into a token, using the
 * specified key.
 *
 * returns WA_ERR_NONE or an error.
 *
 * errors:
 *  WA_ERR_NO_ROOM
 *  WA_ERR_NO_MEM
 *  WA_ERR_BAD_KEY
 *  
 */
int webauth_token_create_with_key(const WEBAUTH_ATTR_LIST *list,
                                  time_t hint,
                                  unsigned char *output,
                                  int *output_len,
                                  int max_output_len,
                                  const WEBAUTH_KEY *key);

/*
 * base64 decodes and decrypts attrs into a token
 * input buffer is modified. The best decryption key
 * on the ring will be tried first, and if that fails
 * all the remaining keys will be tried.
 *
 * list will point to the dynamically-allocated list
 * of attrs and must be freed when no longer needed.
 *
 * returns WA_ERR_NONE on success.
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

/*
 * base64 decodes and decrypts attrs into a token
 * input buffer is modified.
 *
 * list will point to the dynamically-allocated list
 * of attrs and must be freed when no longer needed.
 *
 * returns WA_ERR_NONE on success.
 *
 * errors:
 *  WA_ERR_NO_MEM
 *  WA_ERR_CORRUPT
 *  WA_ERR_BAD_HMAC
 *  WA_ERR_BAD_KEY
 */

int webauth_token_parse_with_key(unsigned char *input,
                                 int input_len,
                                 WEBAUTH_ATTR_LIST **list,
                                 const WEBAUTH_KEY *key);

/******************** krb5 ********************/

/*
 * create new webauth krb5 context for use with all the webauth_krb5_*
 * calls. context must be freed with webauth_krb5_free when finished.
 * one of the various webauth_krb5_init_via* calls should be made
 * before the context is fully usable, except when using webauth_krb5_rd_req.
 *
 * if this call return WA_ERR_KRB5, then the only calls that
 * can be made using the context are webauth_krb5_error_code
 * and webauth_krb5_error_message. The context still needs
 * to be freed.
 *
 */
int webauth_krb5_new(WEBAUTH_KRB5_CTXT **ctxt);

/*
 * causes webauth_krb5_free to close the credential cache
 * instead of destroying it.This call
 * is only useful when you need a file-based cache to 
 * remain intact after a call to webauth_krb5_free.
 */
int
webauth_krb5_keep_cred_cache(WEBAUTH_KRB5_CTXT *context);


/*
 * frees a context. If the cred cache hasn't been closed, then it
 * will be destroyed.
 */
int webauth_krb5_free(WEBAUTH_KRB5_CTXT *context);


/*
 * returns the internal kerberos error code from the last kerberos call,
 * or 0 if there wasn't any error.
 */
int webauth_krb5_error_code(WEBAUTH_KRB5_CTXT *content);

/*
 * returns an error message from the last kerberos call, or the
 * string "success" if there error code was 0.
 * The returned string points to internal storage and
 * does not need to be freed.
 */
const char *webauth_krb5_error_message(WEBAUTH_KRB5_CTXT *content);

/*
 * initialize a context with username/password to obtain a TGT.
 * The TGT is verified using the specified keytab. 
 * The TGT will be placed in the specified cache, or a memory cache 
 * if cache_name is NULL.
 */
int webauth_krb5_init_via_password(WEBAUTH_KRB5_CTXT *context,
                                   const char *username,
                                   const char *password,
                                   const char *keytab,
                                   const char *cache_name);
/*
 * initialize a context with a keytab. Credentials
 * will be placed in the specified cache, or a memory cache if cache_name
 * is NULL.
 */
int webauth_krb5_init_via_keytab(WEBAUTH_KRB5_CTXT *context, 
                                 const char *path,
                                 const char *cache_name);

/*
 * initialize a context with a tgt that was created via 
 * webauth_krb5_export_tgt.
 */
int webauth_krb5_init_via_tgt(WEBAUTH_KRB5_CTXT *context,
                              unsigned char *tgt,
                              int tgt_len,
                              const char *cache_name);

/*
 * export the TGT from the context. Used in constructing a proxy-token
 * after a call to webauth_krb5_init_via_password or webauth_krb5_init_via_tgt.
 *
 * memory returned in tgt should be freed when it is no longer needed.
 */
int webauth_krb5_export_tgt(WEBAUTH_KRB5_CTXT *context,
                            unsigned char **tgt,
                            int *tgt_len,
                            time_t *expiration);

/*
 * import a ticket that was exported via webauth_krb5_export_ticket.
 */
int webauth_krb5_import_ticket(WEBAUTH_KRB5_CTXT *context,
                               unsigned char *ticket,
                               int ticket_len);
/*
 * create a service principal (service/hostname.do.main@realm) 
 * from a service and hostname. If service is NULL, "host" is used,
 * and if hostname is NULL, the local hsotname is used.
 *
 * server_principal should be freed when it is no longer needed.
 */
int webauth_krb5_service_principal(WEBAUTH_KRB5_CTXT *context,
                                   const char *service,
                                   const char *hostname,
                                   char **server_principal);
/*
 * get the principal from the context. should only be called 
 * after a succesfull call to webauth_krb5_init_via*.
 *
 * principal should be freed when it is no longer needed.
 */
int webauth_krb5_get_principal(WEBAUTH_KRB5_CTXT *context,
                               char **principal);
/*
 * export a ticket for the given server_principal. ticket should
 * be freed when no longer needed.
 *
 * should only be called after one of the webauth_krb5_init_via* methods
 * has been successfully called.

 */
int webauth_krb5_export_ticket(WEBAUTH_KRB5_CTXT *context,
                               char *server_principal,
                               unsigned char **ticket,
                               int *ticket_len,
                               time_t *expiration);

/*
 * calls krb5_mk_req using the specified service, and stores the
 * resulting req in req, which should be freed when it is no longer
 * needed.
 *
 * should only be called after one of the webauth_krb5_init_via* methods
 * has been successfully called.
 */
int webauth_krb5_mk_req(WEBAUTH_KRB5_CTXT *context,
                        const char *server_principal,
                        unsigned char **req,
                        int *length);

/*
 * calls krb5_rd_req on the specified request, and returns the
 * client principal in client_principal on success.
 * client_principal should be freed when it is no longer
 * needed.
 *
 * can be called anytime after calling webauth_krb5_new.
 */

int webauth_krb5_rd_req(WEBAUTH_KRB5_CTXT *context,
                        const unsigned char *req,
                        int length,
                        const char *keytab,
                        char **client_principal);

#ifdef  __cplusplus
    /*}*/
#endif

/*
**  Local variables:
**  mode: c
**  c-basic-offset: 4
**  indent-tabs-mode: nil
**  end:
*/

#endif
