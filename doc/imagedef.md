Images definition
-----------------

 * `repos` array of strings, mandatory: Names of yum repositories to
   use, from `.repo` files in the same directory as the image definition.

 * `images`: object, mandatory: Each member defines an image to generate.

Image
-----

 * `packages`: Array of strings, mandatory: Set of packages which should
    be in the image.


Example
-------

{
  "repos": ["fedora-rawhide", "username/my-cool-copr"],

  "maintainer": "Colin Walters <walters@redhat.com>"

  "images": {

    "myapp1": {
      "packages": ["myapp1", "bash"]

      

      "cmd": "/usr/bin/foo"
    },

    "myapp2": {
      "packages": ["myapp2"]
    }
  }
}
