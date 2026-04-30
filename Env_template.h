#ifndef ENV_H
#define ENV_H

/**
 * Credenziali di rete e token OAuth.
 *
 * IMPORTANTE: do not commit this edited file.
 *
 * Qui vivono i segreti (password, API key, client secret, refresh
 * token) + la posizione GPS (dato personale sensibile). Le restanti
 * costanti di dominio stanno nei moduli consumer:
 *   - Calendar.h: CAL_POSIX_TZ (fuso + DST automatico),
 *                 CAL_MSGRAPH_TENANT_ID (tenant Azure AD)
 */

#define WIFI_SSID       "my_wifi"
#define WIFI_PASSWORD   "my_password"

/** API key personale registrata su https://openweathermap.org/api */
#define OWM_API_KEY     ""
#define LAT             40.0000f 
#define LON             10.0000f

/**
 * Credenziali della rete AP che il dispositivo espone al boot per la
 * finestra di aggiornamento OTA (durata OTA_WINDOW_MIN, default 3 min).
 * La password deve essere lunga almeno 8 caratteri perchè WPA2 lo richiede.
 */
#define OTA_AP_SSID     "ePaper-OTA"
#define OTA_AP_PASSWORD "epaper"

/**
 * Credenziali Microsoft Graph per leggere il calendario Outlook.
 * Il flusso è "refresh token"-based: il dispositivo non fa login
 * interattivo, ma usa un refresh token ottenuto una tantum da PC.
 *
 * Come ricavare i valori:
 *  1. Registrare un'app su https://entra.microsoft.com (Azure AD) come
 *     client pubblico (mobile/desktop).
 *  2. Aggiungere delegated permission "Calendars.Read" e "offline_access".
 *  3. Eseguire un auth-code-flow su PC per ottenere il refresh token
 *     (es. script Python con MSAL) e incollarlo qui sotto.
 *
 * Il TENANT_ID non è un segreto: sta in Calendar.h come
 * CAL_MSGRAPH_TENANT_ID.
 */
#define MSGRAPH_CLIENT_ID     "00000000-0000-0000-0000-000000000000"
#define MSGRAPH_REFRESH_TOKEN "paste_refresh_token_here"

/**
 * Credenziali Google Calendar API (OAuth2 refresh-token flow).
 *
 * Come ricavare i valori:
 *  1. Google Cloud Console -> crea progetto -> abilita "Google Calendar API".
 *  2. OAuth consent screen -> External, aggiungi scope
 *     "https://www.googleapis.com/auth/calendar.readonly".
 *  3. Crea credenziali OAuth 2.0 Client ID di tipo "Desktop app":
 *     annota CLIENT_ID e CLIENT_SECRET.
 *  4. Esegui un auth-code flow su PC (es. script Python con
 *     google-auth-oauthlib) per ottenere il refresh token e incollalo.
 */
#define GOOGLE_CLIENT_ID      "0000000000-xxxxxxxxxxxxxxxxxxxxxxx.apps.googleusercontent.com"
#define GOOGLE_CLIENT_SECRET  "paste_client_secret_here"
#define GOOGLE_REFRESH_TOKEN  "paste_refresh_token_here"

#endif
