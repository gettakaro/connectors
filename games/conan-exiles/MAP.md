# Conan Exiles map background

On a Takaro with the protocol 1 game map, the map shows your players on a grid out of the box. The
server files contain no map picture and the connector ships none, so for the in-game map as
background upload it once in Takaro: open the game server's map, pick **The Exiled Lands** (or
**Isle of Siptah**) and upload the image. Do not enter any bounds; the connector already gives
Takaro the exact ones.

Which image: the game's own full-screen map texture from a Conan Exiles **client** install, exported
as PNG or JPEG with an Unreal asset viewer such as FModel:

| Map | Texture (client paks) |
|---|---|
| The Exiled Lands | `ConanSandbox/Content/UI/Textures/GUIs/Map/T_FullscreenMap` (4000 × 4000) |
| Isle of Siptah | `ConanSandbox/Content/DLC/DLC_Siptah/UI/Map/T_Map_Siptah` |

Upload the whole texture: no cropping, rotating or added borders. Takaro stretches the image over the
map's bounds with the top-left pixel at the top-left (north-west) corner, so a smaller copy of the
same full image works too.
