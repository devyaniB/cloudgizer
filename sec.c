/*
Copyright (c) 2017 DaSoftver LLC.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

// 
// Security-related functions for CLD and run-time
// (getting db credentials, encryption etc)
//


#include "cld.h"



// 
// Return hash value of 'val'.
// This is 256 SHA hash.
// The result is a zero-terminated string in hex representation - 64 bytes + zero byte.
//
char *cld_sha( const char *val )
{
    unsigned char hash[SHA256_DIGEST_LENGTH + 1];
    SHA256_CTX sha256;

    SHA256_Init(&sha256);

    int msg_length = strlen( val );

    char *out = (char *) cld_malloc( sizeof(char) * (((SHA256_DIGEST_LENGTH + 1)*2)+1) );
    if (out == NULL) return NULL;

    char *p = out;
    SHA256_Update(&sha256, val, msg_length);
    SHA256_Final(hash, &sha256);

    int i;
    for ( i = 0; i < SHA256_DIGEST_LENGTH; i++, p += 2 ) 
    {
        snprintf ( p, 3, "%02x", hash[i] );
    }
    return out;
}

// 
// Produce a key out of password, and fill cipher context with the key
// Then use this context to actually encrypt or decrypt.
// 'password' is the password to encrypt with. 'e_ctx' is encrypt context
// (used to encrypt) and 'd_ctx' is decrypt context (used to decrypt).
// 'salt' is the salt used. 
// Either e_ctx or d_ctx can be NULL (if we're only encrypting or decrypting).
// Returns 0 if cannot produce the context, 1 if okay.
//
int cld_get_enc_key(const char *password, const char *salt, EVP_CIPHER_CTX *e_ctx, 
             EVP_CIPHER_CTX *d_ctx)
{
    CLD_TRACE("");

    //
    // this is to make sure we add algorithms only once
    // This static is fine. If algos are added once (for whatever module), they are added for all other
    // modules, who use the very same encryption algorithms.
    //
    static int algos_added = 0;

    // if there's salt, it MUST be CLD_SALT_LEN bytes long
    assert (salt == NULL || salt[0] == 0 || strlen(salt)==CLD_SALT_LEN);

    const EVP_CIPHER *cipher;
    const EVP_MD *dgst = NULL;
    unsigned char key[EVP_MAX_KEY_LENGTH], iv[EVP_MAX_IV_LENGTH];

    if (algos_added == 0)
    {
        OpenSSL_add_all_algorithms();
        algos_added = 1;
    }

    cipher = EVP_get_cipherbyname("aes-256-cbc");
    if(!cipher) { return 0;}

    dgst=EVP_get_digestbyname("sha256");
    if(!dgst) { return 0;}

    if(!EVP_BytesToKey(EVP_aes_256_cbc(), EVP_sha256(), salt!=NULL ? (salt[0]==0 ? NULL : (unsigned char *)salt) : NULL,
          (unsigned char *) password,
          strlen(password), 2, key, iv))
    {
        return 0;
    }

    if (e_ctx != NULL)
    {
        EVP_CIPHER_CTX_init(e_ctx);
        EVP_EncryptInit_ex(e_ctx, EVP_aes_256_cbc(), NULL, key, iv);
    }
    if (d_ctx != NULL)
    {
        EVP_CIPHER_CTX_init(d_ctx);
        EVP_DecryptInit_ex(d_ctx, EVP_aes_256_cbc(), NULL, key, iv);
    }

    return 1;
             
}

// 
// Encrypt string 'plaintext' and return encrypted value, with output parameter 
// 'len' being the length of this encrypted value returned by the function. *len is
// the length of data to encrypt, and also the output length after being done (excluding zero byte at the end).
// is_binary is 1 if the encrypted value is binary and not hex-string, otherwise 0.
// We allocate new memory for encrypted value - caller must de-allocate (this is cld_
// allocation, so you may just leave it to be collected by CLD memory garbage collector.
// 'e' is the encryption context, produced as e_ctx in cld_get_enc_key().
// The maximum length of encrypted data is 2*(input_len+AES_BLOCK_SIZE)+1. Note that 
// AES_BLOCK_SIZE is always 16 bytes. So the maximum is 2*(input_len+16)+1 for the purposes
// of sizing.
//
char *cld_aes_encrypt(EVP_CIPHER_CTX *e, const unsigned char *plaintext, int *len, int is_binary)
{
    /* maximum length for encrypted value is *len + AES_BLOCK_SIZE -1 */
    int p_len = *len + AES_BLOCK_SIZE;
    int f_len = 0;
    unsigned char *ciphertext = cld_malloc(p_len+1); 
           
    // reuse the values already set, so same context can be reused for multiple encryptions
    EVP_EncryptInit_ex(e, NULL, NULL, NULL, NULL);
                
    /* update ciphertext, p_len is filled with the length of encrypted text
      len is the size of plaintext in bytes */
    EVP_EncryptUpdate(e, ciphertext, &p_len, plaintext, *len);
                     
    /* add to ciphertext the final remaining bytes */
    EVP_EncryptFinal_ex(e, ciphertext+p_len, &f_len);
                          
    *len = p_len + f_len;

    if (is_binary == 0)
    {
        //
        // Make encrypted text as hex-string for db storage
        //
        char *hex_ciphertext = cld_malloc(2*(*len)+1); 
        int i;
        //
        // Update progress by 2 bytes in hex-string mode
        //
        int tot_len = 0;

        for (i = 0; i < *len; i++)
        {
            CLD_HEX_FROM_BYTE (hex_ciphertext+tot_len, (unsigned int)ciphertext[i]);
            tot_len += 2;
        }

        hex_ciphertext[*len = tot_len] = 0;
        cld_free (ciphertext); // free binary encrypted value
        return hex_ciphertext; // return hex value
    }
    else
    {
        //
        // The encrypted value is binary, not a hex-string, typically for files
        //
        return (char*)ciphertext;
    }

    return ""; // no purpose, eliminate compiler warning about non-return from function
}


// 
// Decrypt string 'ciphertext' and return decrypted value, with output parameter 
// 'len' being the length of this decrypted value returned by the function (excluding zero byte at the end -
// the decrypted value doesn't have to be a string, but it will have a zero byte at the end).
// 'len' is also input parameter - it's the length of input data to decrypt.
// We allocate new memory for decrypted value - caller must de-allocate (this is cld_
// allocation, so you may just leave it to be collected by CLD memory garbage collector.
// 'e' is the encryption context, produced as d_ctx in cld_get_enc_key().
// 'is_binary' is 1 if encrypted value was encrypted in binary mode, and 0 if as a hex string.
// Note that ciphertext must be the result of cld_aes_encrypt() 
//
char *cld_aes_decrypt(EVP_CIPHER_CTX *e, unsigned char *ciphertext, int *len, int is_binary)
{
   if (is_binary == 0)
   {
       //
       // convert ciphertext from hex back to binary
       // (if the encrypted value is hex-string)
       //
       int i;
       int curr_byte = 0;
       for (i = 0; i < *len; i+=2)
       {
           ciphertext[curr_byte] = ((unsigned int)(CLD_CHAR_FROM_HEX(ciphertext[i]))<<4) + (unsigned int)CLD_CHAR_FROM_HEX(ciphertext[i+1]);
           curr_byte++;
       }
       ciphertext[*len = curr_byte] = 0;
   }


   /* plaintext is equal or lesser length than that of ciphertext*/
   int p_len = *len, f_len = 0;
   unsigned char *plaintext = cld_malloc(p_len);
           
   // reuse decryption context in case multiple decryptions done with the same context
   EVP_DecryptInit_ex(e, NULL, NULL, NULL, NULL);
   // decrypt data
   EVP_DecryptUpdate(e, plaintext, &p_len, ciphertext, *len);
   EVP_DecryptFinal_ex(e, plaintext+p_len, &f_len);
                  
   *len = p_len + f_len;
   plaintext[*len] = 0; 
   return (char*)plaintext;
}




//
// Make random string based on current time as random generator for srand().
// 'rnd' is the output, and rnd_len is its length - rnd buffer must have at least
// rnd_len+1 bytes of storage allocated on input.
// Generated random string does ends with zero byte
// (i.e. it is a valid string. The result
// is the alphanumeric string with "!@#$%^&*()_+-=[];<>?" also in the mix.
//
void cld_make_random (char *rnd, int rnd_len)
{
    CLD_TRACE("");
    assert(rnd_len>1);

    // Ensure next call to this function has a bit more random seed because time() will return the same value within
    // the same second
    // This is okay as static, even if this value is carried over from request to request, serving different modules,
    // and as it is, it actually increases the randomness of the result.
    static unsigned int previous_rnd = 0;

    char range[] = "0123456789!@#$%^&*()_+-=[];<>?abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

    srand ((unsigned int)time(NULL)+previous_rnd);
    int i;
    for (i = 0; i<rnd_len-1; i++)
    {
        int val = (((double)rand())/RAND_MAX) * (sizeof(range) - 1);
        rnd[i] = range[val];
    }
    rnd[i] = 0; // finish with zero for sanity, this is byte rnd[rnd_len]

    previous_rnd = (unsigned int)rnd[0]; // randomize next try
}




