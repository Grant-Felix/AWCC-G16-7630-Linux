# AUR 包（`awcc-g16-7630-linux`）

本目录是给 [AUR](https://aur.archlinux.org/) 用的**配方**：`PKGBUILD` + `.SRCINFO`（+ `LICENSE`）。
AUR 只收配方、不收二进制，预编译包挂在 Release 上（Forgejo / GitHub / Gitee）。
`scripts/package.sh` 与 Forgejo Actions 也会用它打 Arch 预编译包。

## 当前状态：还没发布到 AUR

AUR 现在**暂停新账户注册**（HTTP 503，官方为应对一波自动化账户创建滥发而临时关闭；没有手动
排队，公告只发在 `aur-general` 邮件列表与 Arch 新闻通知上）。本机也还没有 AUR 账号的痕迹，
所以这个包**尚未出现在 AUR**。官方明确提醒**不要针对那个页面写重试脚本**——请以邮件列表与
新闻为准，别去轮询。

发布之前，Arch 用户仍有两条路可走：

```bash
# 1) 用 Release 上的预编译包（三个平台都有，国内走 Gitee）
curl -LO https://github.com/Grant-Felix/AWCC-G16-7630-Linux/releases/download/v26.9.22-3/awcc-g16-7630-linux-26.9.22_3-1-x86_64.pkg.tar.zst
sudo pacman -U awcc-g16-7630-linux-26.9.22_3-1-x86_64.pkg.tar.zst

# 2) 用本仓库里的配方本地构建（想自己编译的话）
cp -r packaging/aur /tmp/awcc-aur && cd /tmp/awcc-aur
makepkg -si
# 注意：构建目录不要带空格——makepkg 的调试参数会被路径里的空格拆断（实测踩到）
```

注册恢复后，按文末「推送到 AUR」那三步推上去即可（`PKGBUILD`、`.SRCINFO`、`LICENSE` 都已备好）。

## 与上游那两个包的关系

AUR 上已有的 `awcc-bin` 与 `awcc-git` 是**上游作者 tr1x_em 本人**维护的（源码是上游
`tr1xem/AWCC`，ImGui + OpenGL 那套）。本包是**另一个代码库的 fork**（GTK4 前端，只针对
Dell G16 7630 验证过），因此按提交准则用区分的包名 `awcc-g16-7630-linux`，并声明
`provides=('awcc')` 与 `conflicts=('awcc' 'awcc-bin' 'awcc-git')`——三者都装
`/usr/bin/awcc`，必须互斥。

## 提交准则对照

准则原文的本地副本在仓库根目录 `AUR提交准则/`（不入库），英文原版见
[AUR submission guidelines](https://wiki.archlinux.org/title/AUR_submission_guidelines)。
下表只列与本包相关的条款与我们的做法。

| 准则条款 | 本包的做法 |
| --- | --- |
| 不打包官方仓库已有的程序 | `awcc` 不在官方仓库（查过 `pacman -Ss awcc`），本项目也不在 |
| 不创建重复的包 | 与上游 `awcc-bin` / `awcc-git` 是不同代码库，包名区分 + provides/conflicts |
| 只接受 x86_64 | `arch=('x86_64')` |
| 不要用 `replaces` | 只用 `conflicts` 与 `provides` |
| 从特定版本源码构建的包不加后缀（`-git` / `-bin`） | 包名不带后缀——构建的是发布 tag 的源码 |
| 不放 makepkg 产物与文件列表 | 只提交 `PKGBUILD`、`.SRCINFO`、`LICENSE`；`*.pkg.tar.zst` 已在 `.gitignore` |
| pkgbase 仓库要有 LICENSE（建议 0BSD） | `LICENSE` 是 0BSD，授权对象是**本配方本身**，不是 AWCC 源码（AWCC 是 GPL-3.0） |
| Maintainer 行写「名字 `<邮箱>`」 | 仓库里只写名字——本仓库约定不放真实邮箱，推送前按下面第 3 步补上 |
| 元数据一改就要重新生成 `.SRCINFO` | 每次改 `PKGBUILD` 都 `makepkg --printsrcinfo > .SRCINFO` 一起提交 |

## 版本号与更新时机

| 情况 | 动什么 |
| --- | --- |
| 上游发了新 tag（如 `v26.9.22-4`） | `pkgver` 改成 `26.9.22_4`、`pkgrel` 重置为 `1`，同时更新 `source` 里的 tag 与校验和 |
| 只改打包方式（依赖、安装路径、编译选项等） | `pkgrel` 加一，`pkgver` 不动 |
| 只修错别字之类的小改 | 两个都不动（准则明确要求） |

Arch 的 `pkgver` 不允许含连字符，所以上游的 `26.9.22-4` 在这里写作 `26.9.22_4`
（`source` 里用 `${pkgver/_/-}` 换回连字符去取 tag 归档）。

## 发新版要做的

```bash
cd packaging/aur
# 1) 改 pkgver
$EDITOR PKGBUILD
# 2) 刷新校验和（源码包是 GitHub 按 tag 生成的 tarball）
updpkgsums          # 没装的话：sudo pacman -S pacman-contrib
# 3) 重新生成 .SRCINFO
makepkg --printsrcinfo > .SRCINFO
# 4) 本地先构建一遍，并做 namcap 检查
makepkg -f
namcap PKGBUILD && namcap ./*.pkg.tar.zst
```

## 推送到 AUR

需要一个 AUR 账号。**为 AUR 单独生成一把密钥**——准则建议不要复用旧密钥，出问题好直接弃用：

```bash
ssh-keygen -f ~/.ssh/aur
```

把 `~/.ssh/aur.pub` 贴到 AUR 网页的 My Account，然后给 `~/.ssh/config` 加上：

```
Host aur.archlinux.org
  IdentityFile ~/.ssh/aur
  User aur
```

首次推送（AUR 只接受 `master` 分支，所以克隆时把默认分支定成 `master`）：

```bash
git -c init.defaultBranch=master clone ssh://aur@aur.archlinux.org/awcc-g16-7630-linux.git
cd awcc-g16-7630-linux
# 准则要求 Maintainer 行带邮箱；仓库这份是占位，替换成你自己的
cp ../PKGBUILD ../.SRCINFO ../LICENSE .
git add PKGBUILD .SRCINFO LICENSE
git commit -m "upgpkg: awcc-g16-7630-linux 26.9.22_4-1"
git push
```

之后用户就能装：

```bash
paru -S awcc-g16-7630-linux
yay  -S awcc-g16-7630-linux
```

按 AUR 惯例，更新用的提交信息写成 `upgpkg: <包名> <pkgver>-<pkgrel>`。

## 维护责任

准则要求维护者别「提交完就不管」：看评论与反馈、随上游改动更新依赖与许可证。升级后
**不要在评论区刷版本更新信息**（会冲淡用户反馈）。不再维护就通过网页 disown。
