# GSW图标生成说明

2026-10-18。使用用户提供的图片作参考，通过内置image_gen工具生成透明PNG；没有使用CLI/API回退。项目内源图为`assets/gsw-icon.png`，Windows资源为`gsw/Assets/GSW.ico`。`scripts/generate-gsw-icon.ps1`仅做确定性的尺寸/格式编码，生成16、20、24、32、40、48、64、128、256像素帧。

最终提示词：

Use case: identity-preserve. Asset type: Windows application icon and 16–32px notification-tray icon for GSO STATUS WIN. Turn the supplied reference into ONE clean square application-icon illustration with a genuinely transparent background. Preserve the recognizable white cat character, black oval eyes, yellow nose, red bow on the upper right of the character's head, and yellow/orange flames rising from the head. Tight centered framing around the head and flames; a small simple teal collar and tiny purple keyboard at bottom can suggest working at a computer, but prioritize a large readable face and bow. Remove the room, chair, desk and monitor background. Use bold crisp dark outlines and simplified flat colors, faithful to the playful hand-drawn character. Keep all flames inside the canvas with breathing room. No lettering, no badge, no watermark, no shadow, no gradients. Strong silhouette and few large shapes that remain legible at 16px. Output one final icon asset, not a sheet or presentation.
