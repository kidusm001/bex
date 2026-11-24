# Releasing bex

`bex` ships through a fully automated GitHub Actions CI/CD pipeline:

- **CI** (`.github/workflows/ci.yml`) runs on every pull request and push to `main`:
  - GCC Debug with `-DBEX_WERROR=ON`, CTest, staged install validation (`desktop-file-validate`, `appstreamcli validate --pedantic`, `tst_rename_adversarial`).
  - Clang Debug with AddressSanitizer + UndefinedBehaviorSanitizer.
  - `tst_graph_upgrade` (single-DPR on PRs, full multi-DPR on `main`).
  - Native package build smoke tests for Ubuntu 24.04 (`.deb`), Debian 12 (`.deb`), Fedora 41 (`.rpm`), and Arch Linux (`makepkg`).
- **Release CD** (`.github/workflows/release.yml`) triggers automatically when a `vX.Y.Z` tag is pushed:
  1. Verifies the tag matches `CMakeLists.txt` and builds a deterministic source tarball (`bex-X.Y.Z.tar.gz`).
  2. Runs the release test gate.
  3. Builds native packages in clean distro containers:
     - `bex_X.Y.Z-1_noble_amd64.deb` (Ubuntu 24.04+ / Mint 22+ / Pop!_OS)
     - `bex_X.Y.Z-1_bookworm_amd64.deb` (Debian 12)
     - `bex-X.Y.Z-1.fc41.x86_64.rpm` + `.src.rpm` (Fedora)
     - `bex-X.Y.Z-1-x86_64.pkg.tar.zst` + `PKGBUILD` + `.SRCINFO` (Arch Linux)
     - `bex-X.Y.Z-x86_64.AppImage` (universal Linux x86_64)
     - `SHA256SUMS.txt`
  4. Publishes all assets to **GitHub Releases**.
  5. Publishes the updated `PKGBUILD` + `.SRCINFO` to the **Arch AUR**.
  6. Submits the `.src.rpm` to **Fedora Copr**.
  7. Updates the **Debian/Ubuntu APT repository** on the `gh-pages` branch.

Jobs whose external credentials are not configured yet (`AUR_SSH_PRIVATE_KEY`, `COPR_CONFIG`, `APT_GPG_PRIVATE_KEY`) skip cleanly with a notice so GitHub Releases always succeed out of the box.

## Cutting a Release

```sh
./scripts/bump-version.sh 1.0.1 --tag
git push origin main --tags
```

`scripts/bump-version.sh` keeps `CMakeLists.txt`, `flake.nix`, `data/io.github.kidusm001.bex.metainfo.xml`, `packaging/aur/PKGBUILD`, `packaging/rpm/bex.spec`, and `packaging/debian/changelog` in sync.

## One-Time Repository Setup

### 1. Arch Linux AUR (`publish-aur`)

1. Create an AUR account at <https://aur.archlinux.org> (if you don't already have one).
2. Generate a dedicated SSH key pair:
   ```sh
   ssh-keygen -t ed25519 -f ~/.ssh/aur-bex -N "" -C "bex-release-ci"
   ```
3. Paste `~/.ssh/aur-bex.pub` into your AUR account SSH keys settings.
4. Add `~/.ssh/aur-bex` as the GitHub repository secret **`AUR_SSH_PRIVATE_KEY`** (Settings → Secrets and variables → Actions).
5. (Optional) Set repository variables `AUR_PACKAGE_NAME` (default: `bex`), `AUR_COMMIT_NAME`, `AUR_COMMIT_EMAIL`.

### 2. Fedora Copr (`publish-copr`)

1. Sign in at <https://copr.fedorainfracloud.org> with a Fedora Account (FAS).
2. Create a project named **`bex`** enabling the Fedora chroots you want (e.g. `fedora-41-x86_64`, `fedora-42-x86_64`, `fedora-rawhide-x86_64`, plus `aarch64` if desired).
3. Copy your API token from <https://copr.fedorainfracloud.org/api/> and add the full INI block as the GitHub repository secret **`COPR_CONFIG`**.
4. (Optional) Set repository variable `COPR_PROJECT` if your FAS username differs from `kidusm001` (format: `<fas-user>/bex`).

### 3. Debian/Ubuntu APT Repository on GitHub Pages (`publish-apt`)

1. After your first tagged release creates the `gh-pages` branch, go to **Settings → Pages** and select **Deploy from a branch** → **`gh-pages`** / **`/ (root)`**.
2. *(Recommended)* For a GPG-signed APT repo (so users don't need `trusted=yes`):
   ```sh
   gpg --quick-generate-key "bex APT Archive <kidusmesfin27@gmail.com>" rsa4096 sign 2y
   gpg --armor --export-secret-keys "bex APT Archive"
   ```
   Save the exported armored private key as GitHub secret **`APT_GPG_PRIVATE_KEY`** (and **`APT_GPG_PASSPHRASE`** if you set one).

### 4. NixOS / Nix (`flake.nix`)

No setup required — `flake.nix` works directly from GitHub on every commit and tag:

```sh
nix run github:kidusm001/bex
```
