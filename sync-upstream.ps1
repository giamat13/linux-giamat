# Pull new commits from torvalds/linux into a fresh local branch, on top of
# master (which carries the local fbdesktop/rootfs/netfilter work). Never
# touches master itself - build/boot-test the new branch, then merge it in
# by hand once it's confirmed good.

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

if (git status --porcelain) {
	Write-Error "working tree is not clean, commit or stash first"
	exit 1
}

$branch = "sync-upstream-$(Get-Date -Format yyyy-MM-dd)"
git rev-parse --verify --quiet $branch 2>$null | Out-Null
if ($LASTEXITCODE -eq 0) {
	Write-Error "branch $branch already exists"
	exit 1
}

git fetch upstream
git checkout -b $branch master

git merge upstream/master --no-edit
if ($LASTEXITCODE -ne 0) {
	Write-Host ""
	Write-Host "Merge conflicts in $branch - resolve them, 'git add' the fixed files, then 'git commit' to finish the merge." -ForegroundColor Yellow
	exit 1
}

Write-Host ""
Write-Host "Branch $branch created with upstream/master merged in." -ForegroundColor Green
Write-Host "Build and boot-test it (in WSL) before merging into master:"
Write-Host "  make -j`$(nproc) bzImage && make -C tools/fbdesktop -j`$(nproc)"
Write-Host "Once it boots clean:"
Write-Host "  git checkout master; git merge $branch"
