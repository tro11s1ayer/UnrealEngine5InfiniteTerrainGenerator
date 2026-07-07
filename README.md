# UnrealEngine5InfiniteTerrainGenerator
This is an infinite terrain generator for Unreal Engine 5.  Please make sure that you are adding this to a C++ project inside of Unreal Engine 5.
This is still a work in progress and many of the features are either experimental or are not currently implemented.

Make sure to add both the Header and Source file to your game's "../Source/(*PROJECT_NAME*)/" directory.
<img width="347" height="339" alt="Screenshot 2026-07-07 175558" src="https://github.com/user-attachments/assets/0dc710c1-50f2-4421-ae36-0640c69c8238" />

If, after adding the .h and .cpp files to your source directory, the files are not visible: make sure to compile using the Live Coding tool:
<img width="1573" height="270" alt="Wyrd55 - Unreal Editor 7_7_2026 5_58_40 PM" src="https://github.com/user-attachments/assets/e45b9fef-a350-4826-a990-a49f8b43171f" />

After compiling and when the C++ class is visible, create a blueprint actor to drop into the world:
<img width="482" height="520" alt="Screenshot 2026-07-07 175221" src="https://github.com/user-attachments/assets/d483e7f8-7d7b-4e9d-a537-d870cca1a0b7" />

In your details panel, navigate to the "Noise" section and insert a float graph:
<img width="1544" height="745" alt="Screenshot 2026-07-07 180620" src="https://github.com/user-attachments/assets/3cf1113d-d69e-4021-acb2-b8e0b2c5d0de" />

Below that you will find the foliage section:
Add an array item and add a Blueprint Actor containing a foliage mesh.<img width="1831" height="278" alt="Screenshot 2026-07-07 180750" src="https://github.com/user-attachments/assets/9b10dfe3-6d51-46c9-8b29-6b1b54cf5b47" />

From there you can set the minimum and maximum height to spawn folaige as well as the slope and the density.

Please provide any feedback and enjoy this drop-in solution.
