/* i18n.js - translations for the Relay Gateway pages (de, en, fr).
 * Usage: t('key', {var: value}); elements with data-i18n / data-i18n-ph
 * are filled by applyI18n(). The choice is remembered in the browser. */
const I18N = {
  de: {
    lang: 'Deutsch',
    edit_names: 'Namen',
    relay_n: 'Relais {n}',
    name_ph: 'Name, z. B. Pumpe',
    name_example: 'Pumpe',
    save: 'Speichern',
    cancel: 'Abbrechen',
    http_hint: 'Schalten per HTTP (state=on, off oder toggle):',
    err_name_dup: 'Name schon vergeben',
    err_name_invalid: 'ungültiger Name (1–32 Zeichen)',
    err_name_full: 'zu viele Namen gespeichert',
    connecting: 'verbinde …',
    no_connection: 'keine Verbindung zum Gateway',
    searching_boards: 'Suche Relaiskarten …',
    no_boards: 'Keine Relaiskarte gefunden. Karte am USB-OTG-Anschluss (direkt oder über einen aktiven Hub) anschließen.',
    boards_count: '{n} Karte(n)',
    usb_count: '{n} USB-Gerät(e)',
    mode_online: 'online',
    mode_connecting: 'verbinde',
    mode_setup: 'Einrichtung',
    board: 'Karte',
    usb_addr: 'USB-Adresse',
    status: 'Status',
    status_none: 'noch keine Antwort',
    status_current: 'aktuell',
    status_ago: 'vor {s} s',
    all_off: 'Alle aus',
    all_on: 'Alle an',
    cmd_ph: 'Befehl, z. B. ID',
    send: 'Senden',
    error: 'Fehler',
    wifi_setup: 'WLAN einrichten',
    tip_home: 'Tipp: Im Browser-Menü „Zum Startbildschirm hinzufügen“ – dann startet die Seite wie eine App.',
    setup_title: 'Relay Gateway einrichten',
    setup_question: 'Mit welchem WLAN soll sich das Gateway verbinden?',
    searching_nets: 'Suche Netzwerke …',
    rescan: 'Erneut suchen',
    only24: 'Nur 2,4-GHz-Netze werden unterstützt.',
    other_net: 'Anderes Netz …',
    ssid_ph: 'Name des WLANs',
    pass_ph: 'Passwort',
    connect: 'Verbinden',
    connecting_to: 'Verbinde mit „{ssid}“ …',
    wait_hint: 'Das dauert bis zu 20 Sekunden. Die Verbindung zum Handy kann dabei kurz abbrechen.',
    done: 'Fertig!',
    done_text: 'Das Gateway ist jetzt im WLAN „{ssid}“ unter dieser Adresse erreichbar:',
    or: 'oder',
    done_hint: 'Verbinde dein Handy wieder mit „{ssid}“ und öffne die Adresse. Im Browser-Menü kannst du die Seite dann zum Startbildschirm hinzufügen – sie startet danach wie eine App.',
    copy: 'Adresse kopieren',
    copied: 'Kopiert',
    address: 'Adresse',
    err_no_ap: 'Netz nicht gefunden',
    err_auth: 'Passwort falsch',
    err_fail: 'Verbindung fehlgeschlagen (Code {code})',
    err_invalid: 'Name fehlt oder Passwort zu kurz (mindestens 8 Zeichen)',
    err_unreachable: 'Gateway nicht erreichbar.',
    err_timeout: 'Keine Antwort. Bitte prüfen, ob das Handy noch mit dem Gateway-WLAN verbunden ist.',
  },
  en: {
    lang: 'English',
    edit_names: 'Names',
    relay_n: 'Relay {n}',
    name_ph: 'Name, e.g. Pump',
    name_example: 'Pump',
    save: 'Save',
    cancel: 'Cancel',
    http_hint: 'Switch via HTTP (state=on, off or toggle):',
    err_name_dup: 'name already used',
    err_name_invalid: 'invalid name (1–32 characters)',
    err_name_full: 'too many names stored',
    connecting: 'connecting …',
    no_connection: 'no connection to the gateway',
    searching_boards: 'Looking for relay boards …',
    no_boards: 'No relay board found. Connect a board to the USB-OTG port (directly or through a powered hub).',
    boards_count: '{n} board(s)',
    usb_count: '{n} USB device(s)',
    mode_online: 'online',
    mode_connecting: 'connecting',
    mode_setup: 'setup',
    board: 'Board',
    usb_addr: 'USB address',
    status: 'Status',
    status_none: 'no answer yet',
    status_current: 'up to date',
    status_ago: '{s} s ago',
    all_off: 'All off',
    all_on: 'All on',
    cmd_ph: 'Command, e.g. ID',
    send: 'Send',
    error: 'Error',
    wifi_setup: 'Set up Wi-Fi',
    tip_home: 'Tip: use “Add to Home screen” in the browser menu to start the page like an app.',
    setup_title: 'Set up Relay Gateway',
    setup_question: 'Which Wi-Fi network should the gateway connect to?',
    searching_nets: 'Searching for networks …',
    rescan: 'Search again',
    only24: 'Only 2.4 GHz networks are supported.',
    other_net: 'Other network …',
    ssid_ph: 'Network name',
    pass_ph: 'Password',
    connect: 'Connect',
    connecting_to: 'Connecting to “{ssid}” …',
    wait_hint: 'This takes up to 20 seconds. The connection to your phone may drop briefly.',
    done: 'Done!',
    done_text: 'The gateway is now connected to “{ssid}” and can be reached at:',
    or: 'or',
    done_hint: 'Reconnect your phone to “{ssid}” and open the address. You can then add the page to your home screen in the browser menu – it will start like an app.',
    copy: 'Copy address',
    copied: 'Copied',
    address: 'Address',
    err_no_ap: 'Network not found',
    err_auth: 'Wrong password',
    err_fail: 'Connection failed (code {code})',
    err_invalid: 'Name missing or password too short (at least 8 characters)',
    err_unreachable: 'Gateway not reachable.',
    err_timeout: 'No answer. Please check that your phone is still connected to the gateway Wi-Fi.',
  },
  fr: {
    lang: 'Français',
    edit_names: 'Noms',
    relay_n: 'Relais {n}',
    name_ph: 'Nom, p. ex. Pompe',
    name_example: 'Pompe',
    save: 'Enregistrer',
    cancel: 'Annuler',
    http_hint: 'Commande par HTTP (state=on, off ou toggle) :',
    err_name_dup: 'nom déjà utilisé',
    err_name_invalid: 'nom invalide (1 à 32 caractères)',
    err_name_full: 'trop de noms enregistrés',
    connecting: 'connexion …',
    no_connection: 'pas de connexion avec la passerelle',
    searching_boards: 'Recherche des cartes relais …',
    no_boards: 'Aucune carte relais trouvée. Branchez une carte sur le port USB-OTG (directement ou via un hub alimenté).',
    boards_count: '{n} carte(s)',
    usb_count: '{n} appareil(s) USB',
    mode_online: 'en ligne',
    mode_connecting: 'connexion',
    mode_setup: 'configuration',
    board: 'Carte',
    usb_addr: 'Adresse USB',
    status: 'État',
    status_none: 'pas encore de réponse',
    status_current: 'à jour',
    status_ago: 'il y a {s} s',
    all_off: 'Tout éteindre',
    all_on: 'Tout allumer',
    cmd_ph: 'Commande, p. ex. ID',
    send: 'Envoyer',
    error: 'Erreur',
    wifi_setup: 'Configurer le Wi-Fi',
    tip_home: 'Astuce : « Ajouter à l’écran d’accueil » dans le menu du navigateur – la page démarre alors comme une application.',
    setup_title: 'Configurer Relay Gateway',
    setup_question: 'À quel réseau Wi-Fi la passerelle doit-elle se connecter ?',
    searching_nets: 'Recherche des réseaux …',
    rescan: 'Rechercher à nouveau',
    only24: 'Seuls les réseaux 2,4 GHz sont pris en charge.',
    other_net: 'Autre réseau …',
    ssid_ph: 'Nom du réseau',
    pass_ph: 'Mot de passe',
    connect: 'Se connecter',
    connecting_to: 'Connexion à « {ssid} » …',
    wait_hint: 'Cela peut prendre jusqu’à 20 secondes. La connexion avec le téléphone peut être brièvement interrompue.',
    done: 'Terminé !',
    done_text: 'La passerelle est maintenant connectée à « {ssid} » et accessible à cette adresse :',
    or: 'ou',
    done_hint: 'Reconnectez votre téléphone à « {ssid} » et ouvrez l’adresse. Dans le menu du navigateur, vous pouvez ensuite ajouter la page à l’écran d’accueil – elle démarrera comme une application.',
    copy: 'Copier l’adresse',
    copied: 'Copié',
    address: 'Adresse',
    err_no_ap: 'Réseau introuvable',
    err_auth: 'Mot de passe incorrect',
    err_fail: 'Échec de la connexion (code {code})',
    err_invalid: 'Nom manquant ou mot de passe trop court (8 caractères minimum)',
    err_unreachable: 'Passerelle injoignable.',
    err_timeout: 'Pas de réponse. Vérifiez que le téléphone est toujours connecté au Wi-Fi de la passerelle.',
  },
};

let LANG = (() => {
  try {
    const saved = localStorage.getItem('lang');
    if (saved && I18N[saved]) return saved;
  } catch (e) { /* storage blocked */ }
  const nav = (navigator.language || 'de').slice(0, 2).toLowerCase();
  return I18N[nav] ? nav : 'en';
})();

function t(key, vars) {
  let s = (I18N[LANG] && I18N[LANG][key]) || I18N.de[key] || key;
  if (vars) for (const k in vars) s = s.split('{' + k + '}').join(vars[k]);
  return s;
}

/* Server error codes ("no_ap", "auth", "fail:15", "invalid") -> text */
function tErr(code) {
  if (!code) return '';
  const m = /^fail:(\d+)$/.exec(code);
  if (m) return t('err_fail', { code: m[1] });
  return I18N.de['err_' + code] ? t('err_' + code) : code;
}

function applyI18n() {
  document.documentElement.lang = LANG;
  document.querySelectorAll('[data-i18n]').forEach(el => { el.textContent = t(el.dataset.i18n); });
  document.querySelectorAll('[data-i18n-ph]').forEach(el => { el.placeholder = t(el.dataset.i18nPh); });
}

/* Fills a <select> with the languages; onChange is called after switching. */
function langSelect(sel, onChange) {
  sel.innerHTML = Object.keys(I18N).map(k =>
    `<option value="${k}"${k === LANG ? ' selected' : ''}>${I18N[k].lang}</option>`).join('');
  sel.addEventListener('change', () => {
    LANG = sel.value;
    try { localStorage.setItem('lang', LANG); } catch (e) { /* ignore */ }
    applyI18n();
    if (onChange) onChange();
  });
}
