# BSDisplayControl

## Flatpak build instructions

- Install flatpak

  `sudo yum install flatpak`

- Change director to flatpack

- Use the `com.chandanbsd.bsdisplaycontrol.yml`

- Install dependencies from Flathub

`flatpak-builder build-dir --user --install-deps-from=flathub --download-only com.chandanbsd.bsdisplaycontrol.yml`

- Generate `nuget-sources.json`

`python3 flatpak-dotnet-generator.py --dotnet 9 --freedesktop 23.08 nuget-sources.json ../src/BSDisplayControl.csproj`

- Build and install using Flatpak builder

`flatpak-builder build-dir --user --force-clean --install --repo=repo com.chandanbsd.bsdisplaycontrol.yaml`

- Run the install Flatpak application

`flatpak run com.chandanbsd.bsdisplaycontrol`
