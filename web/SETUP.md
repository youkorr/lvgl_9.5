# ncaseonetwo.xyz — guide de mise en route

Ce projet contient :

```
web/                 ← le site statique (déployé sur GitHub Pages)
worker/              ← proxy Cloudflare Worker qui détient le PAT GitHub
.github/workflows/
  deploy-pages.yml   ← déploie /web sur GitHub Pages
  compile.yml        ← compile ESPHome (stable ou dev) et publie le .bin
```

## ⚠️ Sécurité — à ne JAMAIS faire

- **Ne jamais coller un token GitHub dans un chat, un commit, ou un fichier du repo.**
- **Ne jamais mettre le token dans le JS du navigateur.** Tout visiteur pourrait le voler.
- Le token vit uniquement dans :
  - **Cloudflare Worker** → secret `GH_TOKEN`
  - et/ou dans **GitHub Actions Secrets** si vous utilisez d'autres workflows.

---

## Étape 1 — Créer le repo qui contiendra les builds

Sur https://github.com/new :

- **Owner** : `youkorr`
- **Repository name** : `ncaseonetwo-builds` (ou ce que vous voulez — il faudra l'aligner dans `web/app.js` et `worker/wrangler.toml`)
- **Public** ou Private au choix.

Ensuite, copiez-y :

- le dossier `.github/workflows/compile.yml`

(Le repo `lvgl_9.5` reste utilisé comme **composant externe** dans les YAML générés — c'est différent.)

---

## Étape 2 — Activer GitHub Pages pour le site

Toujours sur GitHub, ouvrez le repo qui contient le dossier `web/` (pour l'instant : `youkorr/lvgl_9.5`).

1. Onglet **Settings → Pages**
2. **Source** : *GitHub Actions*
3. Pushez la branche → le workflow `deploy-pages.yml` se déclenche, et le site est en ligne sur :
   `https://youkorr.github.io/<repo>/`

---

## Étape 3 — Pointer ncaseonetwo.xyz vers GitHub Pages

Chez votre registrar (où vous avez acheté `ncaseonetwo.xyz`), créez les DNS suivants :

| Type  | Name | Value                  |
|-------|------|------------------------|
| A     | @    | 185.199.108.153        |
| A     | @    | 185.199.109.153        |
| A     | @    | 185.199.110.153        |
| A     | @    | 185.199.111.153        |
| CNAME | www  | youkorr.github.io      |

Puis dans **Settings → Pages → Custom domain**, saisir `ncaseonetwo.xyz` et cocher **Enforce HTTPS**.

Le fichier `web/CNAME` est déjà présent — il dit à GitHub Pages que le domaine est `ncaseonetwo.xyz`.

---

## Étape 4 — Créer le PAT GitHub (token, version sûre)

1. https://github.com/settings/tokens?type=beta → **Generate new token (fine-grained)**.
2. **Resource owner** : `youkorr`
3. **Repository access** : *Only select repositories* → uniquement `ncaseonetwo-builds`.
4. **Permissions** :
   - **Actions** : `Read and write`
   - **Contents** : `Read`
   - **Metadata** : `Read`
5. Expiration : 90 jours (renouvelable).
6. **Copiez le token** — il s'affiche **une seule fois**. Vous le collerez à l'étape suivante directement dans Cloudflare, **jamais** dans un fichier ni dans un chat.

---

## Étape 5 — Déployer le Worker Cloudflare

Le Worker est le **seul** endroit où le token est connu.

```bash
cd worker
npm install -g wrangler
wrangler login
wrangler deploy
wrangler secret put GH_TOKEN
# (collez le token ici, dans le terminal — pas ailleurs)
```

Vous obtenez une URL du type `https://ncaseonetwo-api.<sous-domaine>.workers.dev`.

Optionnel : mappez-la sur `api.ncaseonetwo.xyz` via le dashboard Cloudflare (DNS + Workers → Routes).

---

## Étape 6 — Connecter le frontend au Worker

Éditer `web/app.js`, ligne ~10 :

```js
const CONFIG = {
  apiBase:     "https://ncaseonetwo-api.<sous-domaine>.workers.dev",
  compileRepo: "youkorr/ncaseonetwo-builds",
};
```

Commit + push → le site est redéployé automatiquement.

---

## C'est prêt

- Le visiteur drop son YAML → validation locale.
- Il choisit une carte parmi 30 marques (Espressif, M5Stack, LilyGO, Waveshare, Seeed, Adafruit, UM, Wemos).
- Il choisit ESPHome **stable** ou **dev**.
- Bouton *Compile* → Worker → GitHub Actions → `.bin` téléchargeable.

Aucun token n'a jamais été exposé au navigateur.

---

## Tester en local (sans rien déployer)

```bash
cd web
python3 -m http.server 8080
# Ouvrir http://localhost:8080
```

La validation YAML fonctionne immédiatement.
La compilation affichera un message "no backend configured" tant que `CONFIG.apiBase` est vide — c'est normal, c'est volontaire (pas de token côté client).
