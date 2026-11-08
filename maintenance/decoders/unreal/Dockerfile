# takaro-icons-decoder-unreal: CUE4Parse-based exporter for Unreal Engine game files.
# Built locally by `takaro-maint icons build` and never pushed. The network is needed at build time
# only (NuGet restore); the container runs with --network none. Oodle is mounted at run time and is
# not part of the image.
FROM mcr.microsoft.com/dotnet/sdk:10.0@sha256:212fe4cb74627d5adf9817a4088906ae3edff1bd853bb27565d4b46731efe61e AS build
WORKDIR /src
COPY nuget.config TakaroUeExport.csproj packages.lock.json ./
RUN dotnet restore --locked-mode
COPY Program.cs Png.cs ./
RUN dotnet publish -c Release --no-restore -o /app -p:UseAppHost=false

FROM mcr.microsoft.com/dotnet/runtime:10.0@sha256:4f1f30cf0bb4ab0c8b2daa84613051683456f8df4a9bf7c812e20d74962992ac
COPY --from=build /app /app
RUN groupadd --gid 10001 export && useradd --uid 10001 --gid 10001 --no-create-home --shell /usr/sbin/nologin export
USER 10001:10001
ENV DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_EnableDiagnostics=0
ENTRYPOINT ["dotnet", "/app/TakaroUeExport.dll"]
