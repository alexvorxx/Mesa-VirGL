`Mesa <https://mesa3d.org>`_ - The 3D Graphics Library
======================================================


Source
------

This repository lives at https://gitlab.freedesktop.org/mesa/mesa.
Other repositories are likely forks, and code found there is not supported.



Build
---------------

Go to the folder with Mesa code and run the commands:

  $ meson . build -Dgallium-va=disabled -Dgallium-drivers=virgl -Dvulkan-drivers= -Dglx=xlib -Dplatforms=x11 -Dbuildtype=release
 
  $ ninja -C build

Before building earlier versions of Mesa, you need to apply a fix:
"virgl/vtest: fix memory overwrite problem in virgl_vtest_send_get_caps()" 
https://github.com/Mesa3D/mesa/commit/060936fe0de668127310a65e607fa4b7b48d6f0a

Build & install
---------------

You can find more information in our documentation (`docs/install.rst
<https://docs.mesa3d.org/install.html>`_), but the recommended way is to use
Meson (`docs/meson.rst <https://docs.mesa3d.org/meson.html>`_):

.. code-block:: sh

  $ meson setup build
  $ ninja -C build/
  $ sudo ninja -C build/ install


Support
-------

Many Mesa devs hang on IRC; if you're not sure which channel is
appropriate, you should ask your question on `OFTC's #dri-devel
<irc://irc.oftc.net/dri-devel>`_, someone will redirect you if
necessary.
Remember that not everyone is in the same timezone as you, so it might
take a while before someone qualified sees your question.
To figure out who you're talking to, or which nick to ping for your
question, check out `Who's Who on IRC
<https://dri.freedesktop.org/wiki/WhosWho/>`_.

The next best option is to ask your question in an email to the
mailing lists: `mesa-dev\@lists.freedesktop.org
<https://lists.freedesktop.org/mailman/listinfo/mesa-dev>`_


Bug reports
-----------

If you think something isn't working properly, please file a bug report
(`docs/bugs.rst <https://docs.mesa3d.org/bugs.html>`_).


Contributing
------------

Contributions are welcome, and step-by-step instructions can be found in our
documentation (`docs/submittingpatches.rst
<https://docs.mesa3d.org/submittingpatches.html>`_).

Note that Mesa uses gitlab for patches submission, review and discussions.
