# fixture-846

The tracer fixture for `takaro-maint icons`. Every image is synthetic (simple shapes drawn with
Pillow); no game art is involved. `recipes.json` is the recipe document the tests and the staging
publish runbook (`maintenance/docs/icons.md`) build from.

`circle-512-copy.png` has the same pixels as `circle-512.png` saved with a different PNG
`compress_level`: the files differ, the rendered art does not, which proves objects are addressed by
their output bytes.

## Regenerating the images

Run from this directory:

```python
from PIL import Image, ImageDraw

def save(img, name, **kw):
    img.save(f"images/{name}", format="PNG", **kw)

c = Image.new("RGBA", (512, 512), (0, 0, 0, 0)); d = ImageDraw.Draw(c)
d.ellipse((32, 32, 480, 480), fill=(220, 60, 60, 255), outline=(90, 10, 10, 255), width=16)
d.ellipse((160, 160, 352, 352), fill=(250, 220, 90, 255))
save(c, "circle-512.png", compress_level=9)
save(c, "circle-512-copy.png", compress_level=1)

b = Image.new("RGBA", (300, 150), (0, 0, 0, 0)); d = ImageDraw.Draw(b)
d.rectangle((10, 30, 290, 120), fill=(60, 120, 220, 255))
d.polygon([(10, 30), (150, 5), (290, 30)], fill=(30, 60, 140, 255))
save(b, "banner-300x150.png", compress_level=9)

p = Image.new("RGBA", (100, 400), (0, 0, 0, 0)); d = ImageDraw.Draw(p)
d.rectangle((20, 10, 80, 390), fill=(120, 200, 120, 255))
d.rectangle((10, 10, 90, 40), fill=(60, 130, 60, 255))
save(p, "pillar-100x400.png", compress_level=9)

q = Image.new("RGBA", (32, 32), (0, 0, 0, 0)); d = ImageDraw.Draw(q)
d.ellipse((4, 8, 28, 28), fill=(140, 140, 150, 255))
d.ellipse((10, 12, 16, 17), fill=(200, 200, 210, 255))
save(q, "pebble-32.png", compress_level=9)

k = Image.new("RGBA", (64, 64), (0, 0, 0, 0)); d = ImageDraw.Draw(k)
d.rectangle((6, 10, 58, 58), fill=(160, 110, 60, 255), outline=(80, 50, 20, 255), width=3)
d.line((6, 34, 58, 34), fill=(80, 50, 20, 255), width=3)
save(k, "crate-64.png", compress_level=9)
```
