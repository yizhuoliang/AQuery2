import csv
import random

# Number of lines
num_lines = int(input("number of lines: "))

# Generate months in random order
months = list(range(1, num_lines + 1))
random.shuffle(months)

# Generate random prices
prices = [random.randint(1, 2000) for _ in range(num_lines)]

# Write to CSV file
with open('prices.csv', 'w', newline='') as csvfile:
    csvwriter = csv.writer(csvfile)
    csvwriter.writerow(['rownumber', 'month', 'price'])  # Write header
    for rownumber, (month, price) in enumerate(zip(months, prices), start=1):
        csvwriter.writerow([rownumber, month, price])

print(f"CSV file with {num_lines} lines generated as 'prices.csv'")
