Images definition
-----------------

 * `images`: object, mandatory: Each member defines an image to generate.

Image
-----

 * `repos` array of strings, mandatory: Names of yum repositories to
   use, from `.repo` files in the same directory as the treefile.

 * `packages`: Array of strings, mandatory: Set of packages which should
    be in the image.
