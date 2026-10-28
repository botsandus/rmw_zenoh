# Coordinated Hiroz / ROS candidate

Branch `dexory/hiroz-1.10.2` retains the deployed base-topic workaround from
`293c9a98a7a44189cfdf387b15032e57136915bd`: buffer-bearing messages remain on the
ordinary ROS topic keys, allowing Hiroz to interoperate with Images/PointCloud2.

Entity shutdown includes upstream `ros2/rmw_zenoh#994`, applied separately from
the dependency update. It destroys middleware entities on the application thread
and preserves callback draining instead of relying on the older core bypass.

The vendor package pins Zenoh-C `5dd2a5ddd7c6c74cc6228383d368ae3a04d76909` and Zenoh-C++
`20eb0bef670e150d3750ca0739dcb28cd7909e2e`. Zenoh-C pins core
`0acbd21e9358c50e4b129111f024b7576c589889`.
Build both `rmw_zenoh_cpp` and `rmw_zenohd` from this vendor cohort.

This branch is for interoperability qualification. Source pins alone do not
establish a completed ROS/SHM/restart soak. Keep the router and Hiroz on the same
core SHA, use matching native headers/libraries, and record actual image digests.
