# AUR 包（`awcc-g16-7630-linux`）

这是给 [AUR](https://aur.archlinux.org/) 用的 `PKGBUILD`：从 GitHub 上的 release tag
取源码构建（Gitee 是同一份代码的国内镜像）。`scripts/package.sh` 也会用它在本机打
Arch 预编译包（`.pkg.tar.zst`）。

## 版本号映射

Arch 的 `pkgver` **不允许含连字符**，所以上游的日期式版本 `26.9.22-2` 在这里写作
`26.9.22_2`（`pkgrel=1`）。发布新版时改两处：`pkgver` 与 `source` 里的标签
（脚本里用 `${pkgver/_/-}` 自动换回连字符）。

## 发新版要做的

```bash
cd packaging/aur
# 1) 改 pkgver（如 26.9.22_3）
$EDITOR PKGBUILD
# 2) 刷新校验和（源码包是 GitHub 按 tag 生成的 tarball）
updpkgsums          # 没装的话：sudo pacman -S pacman-contrib
# 3) 重新生成 .SRCINFO（AUR 要求提交它）
makepkg --printsrcinfo > .SRCINFO
```

## 推送到 AUR

需要一个 AUR 账号，并把 SSH 公钥加到账号里（`ssh-keygen -t ed25519` 后把
`~/.ssh/id_ed25519.pub` 贴到 AUR 网页端）。之后：

```bash
git clone ssh://aur@aur.archlinux.org/awcc-g16-7630-linux.git
# AUR 上的 Maintainer 行建议换成你自己的真实邮箱（仓库里这份用的是占位地址）
cp PKGBUILD .SRCINFO awcc-g16-7630-linux/
cd awcc-g16-7630-linux
git add PKGBUILD .SRCINFO
git commit -m "awcc-g16-7630-linux 26.9.22_3"
git push
```

推上去之后用户就能：

```bash
paru -S awcc-g16-7630-linux
yay  -S awcc-g16-7630-linux
```

## 为什么仓库里的 PKGBUILD 不用真实邮箱

本仓库不写维护者的真实邮箱（见全局开发规则：邮箱只留在本地 `.git/config`）。AUR 惯例
是写真实邮箱，所以你推送前按上面注释改一下即可。
