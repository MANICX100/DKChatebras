package ai.cerebras.dkchatebras;

import android.content.Context;
import android.content.SharedPreferences;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;

import java.nio.charset.StandardCharsets;
import java.security.KeyStore;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

final class SecureApiKey {
    private static final String ALIAS = "DKChatebrasApiKeyEncryption";
    private static final String VALUE = "api_key_ciphertext";
    private static final String KEYSTORE = "AndroidKeyStore";
    private final SharedPreferences preferences;

    SecureApiKey(Context context) { preferences = context.getSharedPreferences("secure_credentials", Context.MODE_PRIVATE); }
    boolean exists() { return preferences.contains(VALUE); }

    void save(String apiKey) throws Exception {
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, getOrCreateKey());
        String packed = Base64.encodeToString(cipher.getIV(), Base64.NO_WRAP) + "."
                + Base64.encodeToString(cipher.doFinal(apiKey.getBytes(StandardCharsets.UTF_8)), Base64.NO_WRAP);
        if (!preferences.edit().putString(VALUE, packed).commit()) throw new IllegalStateException("Could not store the encrypted API key");
    }

    String read() throws Exception {
        String packed = preferences.getString(VALUE, null);
        if (packed == null) return null;
        String[] parts = packed.split("\\.", 2);
        if (parts.length != 2) throw new IllegalStateException("Stored API key is invalid");
        KeyStore store = KeyStore.getInstance(KEYSTORE); store.load(null);
        SecretKey key = (SecretKey) store.getKey(ALIAS, null);
        if (key == null) throw new IllegalStateException("Encryption key is unavailable; remove and re-add the API key");
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, key, new GCMParameterSpec(128, Base64.decode(parts[0], Base64.NO_WRAP)));
        return new String(cipher.doFinal(Base64.decode(parts[1], Base64.NO_WRAP)), StandardCharsets.UTF_8);
    }

    void remove() throws Exception {
        if (!preferences.edit().remove(VALUE).commit()) throw new IllegalStateException("Could not remove the stored API key");
        KeyStore store = KeyStore.getInstance(KEYSTORE); store.load(null);
        if (store.containsAlias(ALIAS)) store.deleteEntry(ALIAS);
    }

    private SecretKey getOrCreateKey() throws Exception {
        KeyStore store = KeyStore.getInstance(KEYSTORE); store.load(null);
        SecretKey existing = (SecretKey) store.getKey(ALIAS, null);
        if (existing != null) return existing;
        KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE);
        generator.init(new KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256).build());
        return generator.generateKey();
    }
}
