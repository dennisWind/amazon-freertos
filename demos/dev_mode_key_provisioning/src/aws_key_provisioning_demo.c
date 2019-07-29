/* Standard includes. */
#include "string.h"
#include "stdio.h"
#include <stdint.h>

/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "message_buffer.h"

/* Credentials includes. */
#include "aws_clientcredential.h"

/* Demo includes. */
#include "aws_demo_config.h"
#include "aws_key_provisioning_demo.h"

/* Crypto includes. */
#include "iot_pkcs11.h"
#include "iot_pkcs11_config.h"
#include "aws_dev_mode_key_provisioning.h"
#include "iot_tls.h"

/* mbedTLS includes. */
#include "mbedtls/sha256.h"
#include "mbedtls/pk.h"
#include "mbedtls/pk_internal.h"
#include "mbedtls/oid.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509.h"
#include "mbedtls/x509_csr.h"
#include "mbedtls/x509_crt.h"

#ifndef AWS_CLIENT_CREDENTIAL_KEYS_H
    #define AWS_CLIENT_CREDENTIAL_KEYS_H

#endif /* AWS_CLIENT_CREDENTIAL_KEYS_H */

/**
 * @brief Gives users the option to generate keys externally or internally
 * 0 if keys are to be imported
 * 1 if keys are to be generated on device
 */
#define GENERATE_KEYS_ON_DEVICE    ( 1 )

/**
 * @brief Demo is split into two parts -- before CSR/certificate generation, and after
 * 0 to run first part of demo
 * 1 to run second part of demo, after user has copied device and CA certificates into aws_clientcredential_keys.h
 * NOTE: User must change value from 0 to 1 after running scripts (generating certificates) to provision device
 */
#define DEMO_PART                  ( 0 )

/**
 * @brief Gives users the option to reprovision each time the demo is run
 * 0 if no reprovisioning is required
 * 1 if user wants to reprovision the device everytime
 */
/* #define REPROVISION_EACH_TIME 0 */


#define pkcs11configLABEL_DEVICE_PRIVATE_KEY_FOR_TLS    "Device Priv TLS Key"
#define pkcs11testLABEL_DEVICE_PRIVATE_KEY_FOR_TLS      pkcs11configLABEL_DEVICE_PRIVATE_KEY_FOR_TLS

#define pkcs11configLABEL_DEVICE_PUBLIC_KEY_FOR_TLS     "Device Pub TLS Key"
#define pkcs11testLABEL_DEVICE_PUBLIC_KEY_FOR_TLS       pkcs11configLABEL_DEVICE_PUBLIC_KEY_FOR_TLS

#define SHA256_DIGEST_SIZE                              32
#define ECDSA_SIGNATURE_SIZE                            64
#define RSA_SIGNATURE_SIZE                              256
#define MBEDTLS_X509_CSR_H
#define MBEDTLS_X509_CSR_WRITE_C
#define MBEDTLS_PEM_WRITE_C

typedef enum
{
    eNone,                /* Device is not provisioned.  All credentials have been destroyed. */
    eRsaTest,             /* Provisioned using the RSA test credentials located in this file. */
    eEllipticCurveTest,   /* Provisioned using EC test credentials located in this file. */
    eClientCredential,    /* Provisioned using the credentials in aws_clientcredential_keys. */
    eGeneratedEc,         /* Provisioned using elliptic curve generated on device.  Private key unknown.  No corresponding certificate. */
    eGeneratedRsa,
    eDeliberatelyInvalid, /* Provisioned using credentials that are meant to trigger an error condition. */
    eStateUnknown         /* State of the credentials is unknown. */
} CredentialsProvisioned_t;



/* GLOBAL VARIABLES */
CK_SESSION_HANDLE xGlobalSession;
CK_RV xResult;

CK_OBJECT_HANDLE xPrivateKeyHandle = CK_INVALID_HANDLE;
CK_OBJECT_HANDLE xPublicKeyHandle = CK_INVALID_HANDLE;
CK_FUNCTION_LIST_PTR pxGlobalFunctionList;

/*-----------------------------------------------------------*/

/* Declaration of demo function. */
void vStartKeyProvisioningDemo( void );

/*-----------------------------------------------------------*/


/* @brief Random Number Generator Used to Generate CSR
 *
 *
 *	\param[in] pkcs_session     Pointer to PKCS Session Handle
 *
 *  \param[in] pucRandom        Starting Point
 *
 *  \param[in] xRandomLength    Length
 *
 *  \return 0 on success
 *
 */
static int prvRNG( void * pkcs_session,
                   unsigned char * pucRandom,
                   size_t xRandomLength )
{
    BaseType_t xResult;
    CK_FUNCTION_LIST_PTR pxP11FunctionList;

    xResult = C_GetFunctionList( &pxP11FunctionList );

    xResult = pxP11FunctionList->C_GenerateRandom( ( *( ( CK_SESSION_HANDLE * ) pkcs_session ) ), pucRandom, xRandomLength );

    if( xResult != 0 )
    {
        configPRINTF( ( "ERROR: Failed to generate random bytes %d \r\n", xResult ) );
        xResult = TLS_ERROR_RNG;
    }

    return xResult;
}

/*-----------------------------------------------------------*/

/* @brief Alternate Signing Function to be Passed into PK Context Header */
static int prvPrivateKeySigningCallback( void * pvContext,
                                         mbedtls_md_type_t xMdAlg,
                                         const unsigned char * pucHash,
                                         size_t xHashLen,
                                         unsigned char * pucSig,
                                         size_t * pxSigLen,
                                         int ( * piRng )( void *,
                                                          unsigned char *,
                                                          size_t ), /*lint !e955 This parameter is unused. */
                                         void * pvRng )
{
    CK_RV xResult = 0;
    int lFinalResult = 0;
    CK_MECHANISM xMech = { 0 };
    CK_BYTE xToBeSigned[ 256 ];
    uint8_t ucTemp[ 64 ] = { 0 }; /* A temporary buffer for the pre-formatted signature. */
    CK_ULONG xToBeSignedLen = sizeof( xToBeSigned );


    /* Unreferenced parameters. */
    ( void ) ( piRng );
    ( void ) ( pvRng );
    ( void ) ( xMdAlg );

    /* Sanity check buffer length. */
    if( xHashLen > sizeof( xToBeSigned ) )
    {
        xResult = CKR_ARGUMENTS_BAD;
    }

    xMech.mechanism = CKM_ECDSA;
    memcpy( xToBeSigned, pucHash, xHashLen );
    xToBeSignedLen = xHashLen;

    if( 0 == xResult )
    {
        /* Use the PKCS#11 module to sign. */
        xResult = pxGlobalFunctionList->C_SignInit( xGlobalSession,
                                                    &xMech,
                                                    xPrivateKeyHandle );
    }

    if( 0 == xResult )
    {
        *pxSigLen = sizeof( xToBeSigned );
        xResult = pxGlobalFunctionList->C_Sign( ( CK_SESSION_HANDLE ) xGlobalSession,
                                                xToBeSigned,
                                                xToBeSignedLen,
                                                pucSig,
                                                ( CK_ULONG_PTR ) pxSigLen );
    }

    uint8_t * pucSigPtr;

    /* PKCS #11 for P256 returns a 64-byte signature with 32 bytes for R and 32 bytes for S.
     * This must be converted to an ASN1 encoded array. */
    configASSERT( *pxSigLen == 64 );
    memcpy( ucTemp, pucSig, *pxSigLen );

    pucSig[ 0 ] = 0x30; /* Sequence. */
    pucSig[ 1 ] = 0x44; /* The minimum length the signature could be. */
    pucSig[ 2 ] = 0x02; /* Integer. */

    if( ucTemp[ 0 ] & 0x80 )
    {
        pucSig[ 1 ]++;
        pucSig[ 3 ] = 0x21;
        pucSig[ 4 ] = 0x0;
        memcpy( &pucSig[ 5 ], ucTemp, 32 );
        pucSigPtr = pucSig + 33 + 4;
    }
    else
    {
        pucSig[ 3 ] = 0x20;
        memcpy( &pucSig[ 4 ], ucTemp, 32 );
        pucSigPtr = pucSig + 32 + 4;
    }

    pucSigPtr[ 0 ] = 0x02; /* Integer. */
    pucSigPtr++;

    if( ucTemp[ 32 ] & 0x80 )
    {
        pucSig[ 1 ]++;
        pucSigPtr[ 0 ] = 0x21;
        pucSigPtr[ 1 ] = 0x00;
        pucSigPtr += 2;

        memcpy( pucSigPtr, &ucTemp[ 32 ], 32 );
    }
    else
    {
        pucSigPtr[ 0 ] = 0x20;
        pucSigPtr++;
        memcpy( pucSigPtr, &ucTemp[ 32 ], 32 );
    }

    *pxSigLen = ( CK_ULONG ) pucSig[ 1 ] + 2;

    if( xResult != 0 )
    {
        configPRINTF( ( "ERROR: Failure in signing callback: %d \r\n", xResult ) );
        lFinalResult = TLS_ERROR_SIGN;
    }

    return lFinalResult;
}

/*-----------------------------------------------------------*/

/* @brief Provision Device Using Device and CA Certificates
 *
 *
 *	\return    Void
 *  Prints error message upon failure, success message upon success
 *
 */
static void vProvisionDevice( void )
{
    /* Provisioning Device Certificate */
    CK_OBJECT_HANDLE xObject = 0;

    xResult = xProvisionCertificate( xGlobalSession,
                                     keyCLIENT_CERTIFICATE_PEM,
                                     sizeof( keyCLIENT_CERTIFICATE_PEM ),
                                     ( uint8_t * ) pkcs11configLABEL_DEVICE_CERTIFICATE_FOR_TLS,
                                     &xObject );

    if( ( xResult != CKR_OK ) || ( xObject == CK_INVALID_HANDLE ) )
    {
        configPRINTF( ( "ERROR: Failed to provision device certificate. %d \r\n", xResult ) );
    }

    /* Provisioning JITR CA Certificate */
    if( xResult == CKR_OK )
    {
        if( sizeof( keyJITR_DEVICE_CERTIFICATE_AUTHORITY_PEM ) != 0 )
        {
            xResult = xProvisionCertificate( xGlobalSession,
                                             keyJITR_DEVICE_CERTIFICATE_AUTHORITY_PEM,
                                             sizeof( keyJITR_DEVICE_CERTIFICATE_AUTHORITY_PEM ),
                                             ( uint8_t * ) pkcs11configLABEL_JITP_CERTIFICATE,
                                             &xObject );

            xResult = CKR_OK;
        }
    }

    if( xResult == CKR_OK )
    {
        configPRINTF( ( "Device credential provisioning succeeded.\r\n" ) );
    }
    else
    {
        configPRINTF( ( "Device credential provisioning failed.\r\n" ) );
    }
}

/*-----------------------------------------------------------*/

/* @brief Provisions a Device Using Internally Generated Keys
 *
 *
 *  \return Void function, console outputs upon success/failure
 *
 */
static void vDevModeDeviceKeyProvisioning( void )
{
    xResult = xInitializePkcs11Session( &xGlobalSession );

    if( xResult != CKR_OK )
    {
        configPRINTF( ( "Failed to open PKCS #11 session.\r\n" ) );
    }

    xResult = C_GetFunctionList( &pxGlobalFunctionList );

    if( xResult != CKR_OK )
    {
        configPRINTF( ( "Failed to get function list.\r\n" ) );
    }

    #if ( !DEMO_PART )
        {
            CredentialsProvisioned_t xCurrentCredentials = eStateUnknown;

            /* Note that mbedTLS does not permit a signature on all 0's. */
            CK_BYTE xHashedMessage[ SHA256_DIGEST_SIZE ] = { 0xab };
            CK_MECHANISM xMechanism;
            CK_BYTE xSignature[ RSA_SIGNATURE_SIZE ] = { 0 };
            CK_BYTE xEcPoint[ 256 ] = { 0 };
            CK_BYTE xEcParams[ 11 ] = { 0 };
            CK_KEY_TYPE xKeyType;
            CK_ULONG xSignatureLength;
            CK_ATTRIBUTE xTemplate;
            CK_OBJECT_CLASS xClass;

            /* mbedTLS structures for verification. */
            int lMbedTLSResult;
            int ret;
            mbedtls_ecdsa_context xEcdsaContext;
            uint8_t ucSecp256r1Oid[] = pkcs11DER_ENCODED_OID_P256; /*"\x06\x08" MBEDTLS_OID_EC_GRP_SECP256R1; */

            xResult = xDestroyCredentials( xGlobalSession );

            if( xResult != CKR_OK )
            {
                configPRINTF( ( "Failed to destroy credentials before Generating Key Pair.\r\n" ) );
            }

            xCurrentCredentials = eNone;

            xResult = xProvisionGenerateKeyPairEC( xGlobalSession,
                                                   ( uint8_t * ) pkcs11testLABEL_DEVICE_PRIVATE_KEY_FOR_TLS,
                                                   ( uint8_t * ) pkcs11testLABEL_DEVICE_PUBLIC_KEY_FOR_TLS,
                                                   &xPrivateKeyHandle,
                                                   &xPublicKeyHandle );

            if( xResult != CKR_OK )
            {
                configPRINTF( ( "Generating EC key pair failed." ) );
            }

            if( xPrivateKeyHandle == CK_INVALID_HANDLE )
            {
                configPRINTF( ( "Invalid private key handle generated by GenerateKeyPair.\r\n" ) );
            }

            if( xPublicKeyHandle == CK_INVALID_HANDLE )
            {
                configPRINTF( ( "Invalid public key handle generated by GenerateKeyPair.\r\n" ) );
            }

            /* Call GetAttributeValue to retrieve information about the keypair stored. */
            /* Check that correct object class retrieved. */
            xTemplate.type = CKA_CLASS;
            xTemplate.pValue = NULL;
            xTemplate.ulValueLen = 0;
            xResult = pxGlobalFunctionList->C_GetAttributeValue( xGlobalSession, xPublicKeyHandle, &xTemplate, 1 );

            if( xResult != CKR_OK )
            {
                configPRINTF( ( "GetAttributeValue for length of public EC key class failed.\r\n" ) );
            }

            if( xTemplate.ulValueLen != sizeof( CK_OBJECT_CLASS ) )
            {
                configPRINTF( ( "Incorrect object class length returned from GetAttributeValue.\r\n" ) );
            }

            xTemplate.pValue = &xClass;
            xResult = pxGlobalFunctionList->C_GetAttributeValue( xGlobalSession, xPrivateKeyHandle, &xTemplate, 1 );

            if( xResult != CKR_OK )
            {
                configPRINTF( ( "GetAttributeValue for private EC key class failed.\r\n" ) );
            }

            if( xClass != CKO_PRIVATE_KEY )
            {
                configPRINTF( ( "Incorrect object class returned from GetAttributeValue.\r\n" ) );
            }

            xTemplate.pValue = &xClass;
            xResult = pxGlobalFunctionList->C_GetAttributeValue( xGlobalSession, xPublicKeyHandle, &xTemplate, 1 );

            if( xResult != CKR_OK )
            {
                configPRINTF( ( "GetAttributeValue for public EC key class failed.\r\n" ) );
            }

            if( xClass != CKO_PUBLIC_KEY )
            {
                configPRINTF( ( "Incorrect object class returned from GetAttributeValue.\r\n" ) );
            }

            /* Check that both keys are stored as EC Keys. */
            xTemplate.type = CKA_KEY_TYPE;
            xTemplate.pValue = &xKeyType;
            xTemplate.ulValueLen = sizeof( CK_KEY_TYPE );
            xResult = pxGlobalFunctionList->C_GetAttributeValue( xGlobalSession, xPrivateKeyHandle, &xTemplate, 1 );

            if( xResult != CKR_OK )
            {
                configPRINTF( ( "Error getting attribute value of EC key type.\r\n" ) );
            }

            if( xTemplate.ulValueLen != sizeof( CK_KEY_TYPE ) )
            {
                configPRINTF( ( "Length of key type incorrect in GetAttributeValue.\r\n" ) );
            }

            if( xKeyType != CKK_EC )
            {
                configPRINTF( ( "Incorrect key type for private key.\r\n" ) );
            }

            xResult = pxGlobalFunctionList->C_GetAttributeValue( xGlobalSession, xPublicKeyHandle, &xTemplate, 1 );

            if( xResult != CKR_OK )
            {
                configPRINTF( ( "Error getting attribute value of EC key type.\r\n" ) );
            }

            if( xTemplate.ulValueLen != sizeof( CK_KEY_TYPE ) )
            {
                configPRINTF( ( "Length of key type incorrect in GetAttributeValue.\r\n" ) );
            }

            if( xKeyType != CKK_EC )
            {
                configPRINTF( ( "Incorrect key type for public key.\r\n" ) );
            }

            /* Check that public key point can be retrieved for public key. */
            xTemplate.type = CKA_EC_POINT;
            xTemplate.pValue = xEcPoint;
            xTemplate.ulValueLen = sizeof( xEcPoint );
            xResult = pxGlobalFunctionList->C_GetAttributeValue( xGlobalSession, xPublicKeyHandle, &xTemplate, 1 );

            if( xResult != CKR_OK )
            {
                configPRINTF( ( "Failed to retrieve EC Point.\r\n" ) );
            }

            /* Perform a sign with the generated private key. */
            xMechanism.mechanism = CKM_ECDSA;
            xMechanism.pParameter = NULL;
            xMechanism.ulParameterLen = 0;
            xResult = pxGlobalFunctionList->C_SignInit( xGlobalSession, &xMechanism, xPrivateKeyHandle );

            if( xResult != CKR_OK )
            {
                configPRINTF( ( "Failed to SignInit ECDSA.\r\n" ) );
            }

            xSignatureLength = sizeof( xSignature );
            xResult = pxGlobalFunctionList->C_Sign( xGlobalSession, xHashedMessage, SHA256_DIGEST_SIZE, xSignature, &xSignatureLength );

            if( xResult != CKR_OK )
            {
                configPRINTF( ( "Failed to ECDSA Sign.\r\n" ) );
            }

            /* Verify the signature with mbedTLS */
            mbedtls_ecdsa_init( &xEcdsaContext );
            mbedtls_ecp_group_init( &xEcdsaContext.grp );


            lMbedTLSResult = mbedtls_ecp_group_load( &xEcdsaContext.grp, MBEDTLS_ECP_DP_SECP256R1 );

            if( lMbedTLSResult != 0 )
            {
                configPRINTF( ( "mbedTLS failed in setup for signature verification.\r\n" ) );
            }

            /* The first 2 bytes are for ASN1 type/length encoding. */
            lMbedTLSResult = mbedtls_ecp_point_read_binary( &xEcdsaContext.grp, &xEcdsaContext.Q, &xEcPoint[ 2 ], xTemplate.ulValueLen - 2 );

            if( lMbedTLSResult != 0 )
            {
                configPRINTF( ( "mbedTLS failed in setup for signature verification.\r\n" ) );
            }

            /* Generate a PK Context for CSR */
            /* Getting header info from ECKEY type */
            mbedtls_pk_type_t type_key = MBEDTLS_PK_ECKEY;
            const mbedtls_pk_info_t * header = mbedtls_pk_info_from_type( type_key );

            /* Creating copy of header to pass in custom sign function (original header is immutable) */
            mbedtls_pk_info_t * header_copy = pvPortMalloc( sizeof( mbedtls_pk_info_t ) );
            memcpy( header_copy, header, sizeof( struct mbedtls_pk_info_t ) );

            header_copy->sign_func = &prvPrivateKeySigningCallback;

            /* Initializing PK Context */
            mbedtls_pk_context pk_cont;
            mbedtls_pk_init( &pk_cont );
            ret = mbedtls_pk_setup( &pk_cont, header_copy );

            if( ret != 0 )
            {
                configPRINTF( ( "Failed to initialize PK context with given information.\r\n" ) );
            }

            pk_cont.pk_ctx = &xEcdsaContext;


            /* Generating CSR */
            /* Initializing CSR Context */
            mbedtls_x509write_csr my_csr;
            mbedtls_x509write_csr_init( &my_csr );
            ret = mbedtls_x509write_csr_set_subject_name( &my_csr, "CN=ThingName" );

            if( ret != 0 )
            {
                configPRINTF( ( "Failed to set subject name of CSR context.\r\n" ) );
            }

            mbedtls_x509write_csr_set_key( &my_csr, &pk_cont );
            mbedtls_x509write_csr_set_md_alg( &my_csr, MBEDTLS_MD_SHA256 );
            ret = mbedtls_x509write_csr_set_key_usage( &my_csr, MBEDTLS_X509_KU_DIGITAL_SIGNATURE );

            if( ret != 0 )
            {
                configPRINTF( ( "Failed to set key usage of CSR context.\r\n" ) );
            }

            ret = mbedtls_x509write_csr_set_ns_cert_type( &my_csr, MBEDTLS_X509_NS_CERT_TYPE_SSL_CLIENT );

            if( ret != 0 )
            {
                configPRINTF( ( "Failed to set NS Cert Type of CSR context.\r\n" ) );
            }

            /* Output Buffer */
            unsigned char * final_csr = pvPortMalloc( 2000 );
            size_t len_buf = ( size_t ) 2000;

            ret = mbedtls_x509write_csr_pem( &my_csr, final_csr, len_buf, &prvRNG, &xGlobalSession );

            if( ret != 0 )
            {
                configPRINTF( ( "Failed to write CSR.\r\n" ) );
            }

            /* Console Outputs */
            unsigned char * csr_message = "1) PLEASE COPY THE FOLLOWING CERTIFICATE REQUEST INTO tools/create_certs/device_cert.csr :";
            unsigned char * script_message = "2) ONCE YOU'VE COPIED THE CERTIFICATE REQUEST, PLEASE RUN THE SCRIPT NAMED\n\t\t\"device_cert.h\" LOCATED IN tools/create_certs";
            unsigned char * cert_message = "3) ONCE YOU HAVE COMPLETED RUNNING THE SCRIPT, OPEN \"aws_clientcrediental_keys.h\" AND:"                                                                  \
                                           "\n\ta) FORMAT THE TWO CERTIFICATES LOCATED IN tools/create_certs/deviceCertAndCACert.crt\n\t\tUSING tools/certificate_configuration/PEMfileToCString.html" \
                                           "\n\tb) PASTE THE RESULTING TWO C STRINGS INTO\n\t\tkeyCLIENT_CERTIFICATE_PEM AND keyJITR_DEVICE_CERTIFICATE_AUTHORITY_PEM, RESPECTIVELY,\n\t\tIN \"aws_clientcrediental_keys.h\"";

            configPRINTF( ( "\n\n%s\n\n%s\n\n%s\n\n%s\r\n\n", csr_message, final_csr, script_message, cert_message ) );

            /* Freeing Memory */
            mbedtls_ecp_group_free( &xEcdsaContext.grp );
            mbedtls_ecdsa_free( &xEcdsaContext );
            /*mbedtls_pk_free( &pk_cont ); */
            mbedtls_x509write_csr_free( &my_csr );
            vPortFree( final_csr );
        }
    #else  /* if ( !DEMO_PART ) */
        {
            /* Provision device using certificates in aws_clientcredential_keys.h */
            vProvisionDevice();
        }
    #endif /* if ( !DEMO_PART ) */
}


void vStartKeyProvisioningDemo( void )
{
    #if ( !GENERATE_KEYS_ON_DEVICE )
        {
            configPRINTF( ( "Keys are Being Imported\r\n" ) );
            configPRINTF( ( "Starting Key Provisioning\r\n" ) );

            ( void ) vDevModeKeyProvisioning();

            configPRINTF( ( "Ending Key Provisioning\r\n" ) );
        }
    #else
        {
            configPRINTF( ( "Keys Are Being Generated on Device\r\n" ) );
            configPRINTF( ( "Starting Key Provisioning\r\n" ) );

            ( void ) vDevModeDeviceKeyProvisioning();

            configPRINTF( ( "Ending Key Provisioning\r\n" ) );
        }
    #endif /* if ( !GENERATE_KEYS_ON_DEVICE ) */
}
