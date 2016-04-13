# Copyright (C) 2016 Quaternion Risk Management Ltd.
# All rights reserved.

from npvlib import NpvCube
import matplotlib.pyplot as plt
from mpl_toolkits.mplot3d import Axes3D
# import seaborn # slightly improves look of the plots

csv_file = 'cube_full.csv'
npv = NpvCube(csv_file)
trade = '833397AI'

fig = plt.figure()
ax = fig.add_subplot(111, projection='3d')
npv.plot_density(trade, ax, 0.7)
plt.show()
